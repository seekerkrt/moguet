#include "invocation_owned_source_build_context.hpp"

#include "process.hpp"
#include "reviewed_source_acceptance.hpp"
#include "reviewed_source_presentation.hpp"
#include "reviewed_source_review.hpp"
#include "reviewed_source_state_store.hpp"
#include "reviewed_source_trusted_review.hpp"
#include "trusted_cache.hpp"
#include "trusted_cache_test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;

static_assert(!std::is_default_constructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(!std::is_copy_constructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(!std::is_copy_assignable_v<
              InvocationOwnedSourceBuildContext>);
static_assert(std::is_move_constructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(!std::is_move_assignable_v<
              InvocationOwnedSourceBuildContext>);
static_assert(std::is_nothrow_destructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(!std::is_constructible_v<
              InvocationOwnedSourceBuildContext,
              PackageBaseIdentity,
              AurRecipeRevision,
              fs::path,
              fs::path,
              fs::path,
              fs::path>);
static_assert(std::is_invocable_v<
              decltype(create_invocation_owned_source_build_context),
              PinnedReviewedSourceBuild>);
static_assert(!std::is_invocable_v<
              decltype(create_invocation_owned_source_build_context),
              PackageBaseIdentity,
              AurRecipeRevision,
              fs::path>);
static_assert(!std::is_default_constructible_v<
              ReviewedRecipeSnapshotIdentity>);
static_assert(!std::is_constructible_v<
              ReviewedRecipeSnapshotIdentity,
              ReviewedSourceStateRecordBinding,
              ReviewedSourceObjectId,
              std::size_t>);
static_assert(!std::is_default_constructible_v<
              InvocationOwnedMakepkgEnvironment>);
static_assert(!std::is_copy_constructible_v<
              InvocationOwnedMakepkgEnvironment>);
static_assert(std::is_move_constructible_v<
              InvocationOwnedMakepkgEnvironment>);

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
    const Variant& value, std::string_view message) {
    const Expected* arm = std::get_if<Expected>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return *arm;
}

class TemporaryTree final {
public:
    explicit TemporaryTree(std::string_view label) {
        std::string path_template =
            "/tmp/moguet-invocation-context-test-" +
            std::string(label) + "-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::runtime_error("Failed to create test directory");
        }
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
        std::string name, const std::optional<std::string>& value)
        : name_(std::move(name)) {
        const char* previous = std::getenv(name_.c_str());
        if(previous != nullptr) previous_ = previous;
        const int status = value.has_value()
                               ? ::setenv(name_.c_str(), value->c_str(), 1)
                               : ::unsetenv(name_.c_str());
        if(status != 0) {
            throw std::runtime_error("Failed to set test environment");
        }
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
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_AUTHOR_NAME=Slice 3 Fixture",
        "GIT_AUTHOR_EMAIL=slice3@example.invalid",
        "GIT_COMMITTER_NAME=Slice 3 Fixture",
        "GIT_COMMITTER_EMAIL=slice3@example.invalid",
        "GIT_TERMINAL_PROMPT=0",
    };
}

class PinnedBuildFixture final {
public:
    explicit PinnedBuildFixture(
        std::string label,
        bool include_srcinfo = true,
        bool include_symlink = false)
        : tree_(label), package_base_("example-base"),
          remote_url_(
              "https://aur.archlinux.org/" + package_base_ + ".git") {
        cache_home_ = tree_.path() / "cache";
        state_home_ = tree_.path() / "state";
        home_ = tree_.path() / "home";
        fs::create_directory(cache_home_);
        fs::create_directory(state_home_);
        fs::create_directory(home_);
        fs::permissions(
            cache_home_, fs::perms::owner_all,
            fs::perm_options::replace);
        fs::permissions(
            state_home_, fs::perms::owner_all,
            fs::perm_options::replace);
        fs::permissions(
            home_, fs::perms::owner_all,
            fs::perm_options::replace);
        environment_.push_back(std::make_unique<ScopedEnvironmentVariable>(
            "XDG_CACHE_HOME", cache_home_.string()));
        environment_.push_back(std::make_unique<ScopedEnvironmentVariable>(
            "XDG_STATE_HOME", state_home_.string()));
        environment_.push_back(std::make_unique<ScopedEnvironmentVariable>(
            "HOME", home_.string()));

        cache_root_.emplace(prepare_test_trusted_cache_root());
        checkout_.emplace(create_trusted_cache_directory(
            *cache_root_, package_base_));
        repository_ = checkout_->path();

        run_git({"init", "-q", "-b", "main"});
        run_git({"config", "--local", "remote.origin.url", remote_url_});
        run_git({"config", "--local", "remote.origin.fetch",
                 "+refs/heads/*:refs/remotes/origin/*"});
        run_git({"config", "--local", "branch.main.remote", "origin"});
        run_git({"config", "--local", "branch.main.merge",
                 "refs/heads/main"});

        write_file(".gitignore", "ignored-*.tmp\n");
        write_file("PKGBUILD", first_pkgbuild());
        if(include_srcinfo) write_file(".SRCINFO", first_srcinfo());
        write_file("local/source.conf", "reviewed-local-source\n");
        write_file("helpers/build-helper", "#!/bin/sh\nexit 0\n", 0755);
        if(include_symlink) {
            require(
                ::symlink("source.conf", (repository_ / "local/link").c_str()) ==
                    0,
                "Failed to create tracked symlink");
        }
        first_oid_ = commit("first");
        write_file("PKGBUILD", second_pkgbuild());
        second_oid_ = commit("second");
        run_git({"update-ref", "refs/remotes/origin/main", second_oid_});
    }

    [[nodiscard]] const fs::path& repository() const noexcept {
        return repository_;
    }

    [[nodiscard]] const std::string& first_oid() const noexcept {
        return first_oid_;
    }

    [[nodiscard]] const std::string& second_oid() const noexcept {
        return second_oid_;
    }

    [[nodiscard]] const std::string& package_base() const noexcept {
        return package_base_;
    }

    [[nodiscard]] static std::string first_pkgbuild() {
        return "pkgname=example\npkgver=1\npkgrel=1\n"
               "source=(local/source.conf)\n";
    }

    [[nodiscard]] static std::string second_pkgbuild() {
        return "pkgname=example\npkgver=2\npkgrel=1\n"
               "source=(local/source.conf)\n";
    }

    [[nodiscard]] static std::string first_srcinfo() {
        return "pkgbase = example\n\tpkgver = 1\n\tpkgrel = 1\n"
               "\tsource = local/source.conf\npkgname = example\n";
    }

    [[nodiscard]] ValidatedCachePath checkout() const {
        return revalidate_trusted_cache_path(
            *checkout_, CachePathRequirement::ExistingDirectory);
    }

    [[nodiscard]] AurReviewedSourceReviewIdentity identity(
        const std::string& oid) const {
        return AurReviewedSourceReviewIdentity::make(
            PackageBaseIdentity::make(
                PackageSourceIdentity::aur(
                    SourceLocationIdentity::known_git_remote(remote_url_)),
                package_base_),
            SourceRevisionIdentity::git_commit(oid));
    }

    void write_file(
        const std::string& relative_path, std::string_view contents,
        mode_t mode = 0644) const {
        const fs::path path = repository_ / relative_path;
        if(!path.parent_path().empty()) {
            fs::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if(!output) throw std::runtime_error("Failed to open fixture file");
        output.write(
            contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        require(static_cast<bool>(output), "Failed to write fixture file");
        require(::chmod(path.c_str(), mode) == 0,
                "Failed to set fixture file mode");
    }

    [[nodiscard]] std::string read_file(
        const std::string& relative_path) const {
        return read_path(repository_ / relative_path);
    }

    [[nodiscard]] std::string output_git(
        std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", repository_.string()};
        complete.insert(complete.end(), arguments.begin(), arguments.end());
        CapturedCommandResult result = capture_explicit_process_output_raw(
            ExplicitProcessInvocation{
                "/usr/bin/git", std::move(complete),
                git_environment(home_)},
            true);
        if(result.exit_code != 0 || result.stdout_capture_limit_exceeded ||
           result.output.empty() || result.output.back() != '\n') {
            throw std::runtime_error("Fixture Git capture failed");
        }
        result.output.pop_back();
        return result.output;
    }

    void run_git(std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", repository_.string()};
        complete.insert(complete.end(), arguments.begin(), arguments.end());
        const int status = run_explicit_process(
            ExplicitProcessInvocation{
                "/usr/bin/git", std::move(complete),
                git_environment(home_)},
            true, true);
        if(status != 0) {
            throw std::runtime_error("Fixture Git command failed");
        }
    }

private:
    [[nodiscard]] static std::string read_path(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if(!input) throw std::runtime_error("Failed to read fixture file");
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    [[nodiscard]] std::string commit(std::string_view message) const {
        run_git({"add", "-A"});
        run_git({"commit", "-q", "-m", std::string(message)});
        return output_git({"rev-parse", "HEAD"});
    }

    TemporaryTree tree_;
    std::string package_base_;
    std::string remote_url_;
    fs::path cache_home_;
    fs::path state_home_;
    fs::path home_;
    fs::path repository_;
    std::string first_oid_;
    std::string second_oid_;
    std::vector<std::unique_ptr<ScopedEnvironmentVariable>> environment_;
    std::optional<ValidatedCacheRoot> cache_root_;
    std::optional<ValidatedCachePath> checkout_;
};

ExplicitConfirmationResult explicit_yes() {
    ExplicitConfirmationInputParseResult parsed =
        parse_explicit_confirmation_input("yes");
    return take_arm<ExplicitConfirmationAcceptance>(
        parsed, "Explicit yes was not accepted");
}

AcceptedReviewedSourceTarget make_initial_accepted(
    const AurReviewedSourceReviewIdentity& identity) {
    ReviewedSourceLifecyclePlanResult planned =
        plan_reviewed_source_lifecycle(identity);
    ReviewedSourceReviewRequirement requirement =
        take_arm<ReviewedSourceReviewRequirement>(
            planned, "Initial lifecycle did not require review");
    ReviewedSourceVerifiedMaterializedReview verified =
        seal_reviewed_source_materialized_review_for_test(
            ReviewedSourceMaterializedInitialFullReview{
                identity.target_revision(),
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

PinnedReviewedSourceBuild make_pinned_build(
    PinnedBuildFixture& fixture, bool with_editor_overlay = false) {
    const AurReviewedSourceReviewIdentity identity =
        fixture.identity(fixture.first_oid());
    AcceptedReviewedSourceCheckoutResult materialized =
        materialize_accepted_reviewed_source_checkout(
            make_initial_accepted(identity), fixture.checkout());
    AcceptedReviewedSourceCheckout checkout =
        take_arm<AcceptedReviewedSourceCheckout>(
            materialized, "Exact checkout materialization failed");
    if(!with_editor_overlay) {
        ReviewedSourcePublicationResult publication =
            publish_accepted_reviewed_source_checkout(std::move(checkout));
        return take_arm<PinnedReviewedSourceBuild>(
            publication, "Reviewed publication did not produce a pin");
    }

    ReviewedSourceEditorBoundaryResult boundary_result =
        begin_reviewed_source_editor_boundary(checkout);
    ReviewedSourceEditorBoundary boundary =
        take_arm<ReviewedSourceEditorBoundary>(
            boundary_result, "Editor boundary failed");
    fixture.write_file(
        "PKGBUILD", PinnedBuildFixture::first_pkgbuild() +
                        "# invocation editor overlay\n");
    ReviewedSourceEditorOverlayProofResult overlay_result =
        seal_reviewed_source_editor_overlay(
            checkout, std::move(boundary));
    ReviewedSourceEditorOverlayProof overlay =
        take_arm<ReviewedSourceEditorOverlayProof>(
            overlay_result, "Editor overlay seal failed");
    ReviewedSourcePublicationResult publication =
        publish_accepted_reviewed_source_checkout_with_editor_overlay(
            std::move(checkout), std::move(overlay));
    return take_arm<PinnedReviewedSourceBuild>(
        publication, "Overlay publication did not produce a pin");
}

InvocationOwnedSourceBuildContext make_context(
    PinnedBuildFixture& fixture) {
    InvocationOwnedSourceBuildContextResult result =
        create_invocation_owned_source_build_context(
            make_pinned_build(fixture));
    if(const auto* failure =
           std::get_if<InvocationOwnedSourceBuildContextFailure>(&result)) {
        std::ostringstream diagnostic;
        diagnostic << "Context creation failed: stage="
                   << static_cast<int>(failure->stage)
                   << " reason=" << static_cast<int>(failure->reason)
                   << " path=" << failure->relative_path.string();
        if(failure->system_error.has_value()) {
            diagnostic << " errno=" << failure->system_error->value()
                       << " " << failure->system_error->message();
        }
        if(failure->diagnostic.has_value()) {
            diagnostic << " diagnostic=" << *failure->diagnostic;
        }
        throw std::runtime_error(diagnostic.str());
    }
    return take_arm<InvocationOwnedSourceBuildContext>(
        result, "Context creation failed");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) throw std::runtime_error("Failed to read context file");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

mode_t path_mode(const fs::path& path) {
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0, "Failed to inspect path");
    return static_cast<mode_t>(status.st_mode & 07777);
}

constexpr std::string_view CONTEXT_ROOT_PREFIX =
    "moguet-source-build-context-";

std::vector<fs::path> owned_context_root_inventory(
    const fs::path& parent) {
    std::vector<fs::path> roots;
    for(const fs::directory_entry& entry : fs::directory_iterator(parent)) {
        const std::string leaf = entry.path().filename().string();
        if(!leaf.starts_with(CONTEXT_ROOT_PREFIX)) continue;
        struct stat status{};
        require(
            ::lstat(entry.path().c_str(), &status) == 0,
            "Failed to inspect context-root inventory");
        if(S_ISDIR(status.st_mode) && status.st_uid == ::geteuid()) {
            roots.push_back(entry.path());
        }
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

std::vector<std::string> directory_entry_names(const fs::path& directory) {
    std::vector<std::string> names;
    for(const fs::directory_entry& entry :
        fs::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void require_owned_test_directory(
    const fs::path& path, mode_t expected_mode) {
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0,
            "Failed to inspect retained test directory");
    require(S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
                static_cast<mode_t>(status.st_mode & 07777) == expected_mode,
            "Retained test directory lost its exact ownership proof");
}

void require_owned_empty_test_directory(
    const fs::path& path, mode_t expected_mode) {
    require_owned_test_directory(path, expected_mode);
    require(fs::is_empty(path),
            "Retained test directory contains an unexpected entry");
}

mode_t read_process_umask() noexcept {
    const mode_t current = ::umask(0);
    static_cast<void>(::umask(current));
    return current;
}

class ScopedUmask final {
public:
    explicit ScopedUmask(mode_t value) noexcept : previous_(::umask(value)) {
    }

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

    ~ScopedUmask() noexcept {
        static_cast<void>(::umask(previous_));
    }

private:
    mode_t previous_;
};

void require_successful_cleanup(
    InvocationOwnedSourceBuildContext& context,
    const fs::path& root) {
    InvocationOwnedSourceBuildContextCleanupResult cleanup =
        context.cleanup();
    require(
        std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(
            cleanup),
        "Context cleanup failed");
    require(!context.valid(), "Cleaned context remained valid");
    require(!fs::exists(root), "Context root survived cleanup");
}

void test_exact_snapshot_and_physical_separation() {
    PinnedBuildFixture fixture("slice3-exact");
    fixture.write_file("PKGBUILD", "dirty checkout bytes\n");
    fixture.write_file("ignored-residue.tmp", "ignored\n");
    fixture.write_file("ordinary-residue.tmp", "untracked\n");

    InvocationOwnedSourceBuildContext context = make_context(fixture);
    const fs::path root = context.owned_root();
    require(context.valid(), "Fresh context is invalid");
    require(context.recipe_root() != fixture.repository(),
            "Persistent checkout became recipe root");
    require(context.recipe_root().parent_path() == root &&
                context.pkgdest().parent_path() == root &&
                context.builddir().parent_path() == root &&
                context.srcdest().parent_path() == root,
            "Private roots do not share one owned parent");
    require(context.recipe_root() != context.pkgdest() &&
                context.recipe_root() != context.builddir() &&
                context.recipe_root() != context.srcdest() &&
                context.pkgdest() != context.builddir() &&
                context.pkgdest() != context.srcdest() &&
                context.builddir() != context.srcdest(),
            "Private roots are not distinct");
    require(path_mode(root) == 0700 &&
                path_mode(context.pkgdest()) == 0700 &&
                path_mode(context.builddir()) == 0700 &&
                path_mode(context.srcdest()) == 0700 &&
                path_mode(context.recipe_root()) == 0500,
            "Private root modes are not sealed");
    require(
        read_file(context.recipe_root() / "PKGBUILD") ==
            PinnedBuildFixture::first_pkgbuild(),
        "Snapshot did not use exact reviewed PKGBUILD bytes");
    require(
        read_file(context.recipe_root() / ".SRCINFO") ==
            PinnedBuildFixture::first_srcinfo(),
        "Snapshot did not use exact reviewed .SRCINFO bytes");
    require(
        read_file(context.recipe_root() / "local/source.conf") ==
            "reviewed-local-source\n",
        "Tracked local source was not materialized");
    require(path_mode(context.recipe_root() / "helpers/build-helper") == 0500,
            "Executable Git mode was not projected safely");
    require(!fs::exists(context.recipe_root() / ".git") &&
                !fs::exists(context.recipe_root() / "ignored-residue.tmp") &&
                !fs::exists(context.recipe_root() / "ordinary-residue.tmp"),
            "Git metadata or dirty/untracked content entered snapshot");
    require(context.snapshot_identity().tracked_entry_count() == 5,
            "Snapshot tracked entry count changed");
    require(
        context.snapshot_identity().git_tree_object_id().value() ==
            fixture.output_git(
                {"rev-parse", fixture.first_oid() + "^{tree}"}),
        "Snapshot identity is not the exact reviewed Git tree");
    require(
        context.snapshot_identity().reviewed_binding() ==
            context.reviewed_binding(),
        "Snapshot identity lost the #411 record binding");
    require(context.makepkg_executable().path() == "/usr/bin/makepkg" &&
                context.makepkg_executable().inode() != 0,
            "Context did not fix the makepkg executable identity");
    require(
        std::holds_alternative<InvocationOwnedSourceBuildContextValidated>(
            context.revalidate()),
        "Fresh context did not revalidate");

    fixture.write_file("PKGBUILD", "persistent checkout mutation\n");
    require(
        read_file(context.recipe_root() / "PKGBUILD") ==
            PinnedBuildFixture::first_pkgbuild(),
        "Persistent checkout mutation changed the snapshot");

    require(::chmod(context.recipe_root().c_str(), 0700) == 0,
            "Failed to open recipe root for mutation test");
    require(::chmod(
                (context.recipe_root() / "PKGBUILD").c_str(), 0600) == 0,
            "Failed to open snapshot file for mutation test");
    {
        std::ofstream output(
            context.recipe_root() / "PKGBUILD",
            std::ios::binary | std::ios::trunc);
        output << "snapshot mutation\n";
    }
    require(fixture.read_file("PKGBUILD") == "persistent checkout mutation\n",
            "Snapshot mutation changed the persistent checkout");
    require(
        std::holds_alternative<InvocationOwnedSourceBuildContextFailure>(
            context.revalidate()),
        "Snapshot mutation retained context authority");
    require_successful_cleanup(context, root);
}

void test_environment_precedence_unique_roots_and_move_only_lineage() {
    PinnedBuildFixture first_fixture("slice3-first");
    InvocationOwnedSourceBuildContext first = make_context(first_fixture);
    PinnedBuildFixture second_fixture("slice3-second");
    InvocationOwnedSourceBuildContext second = make_context(second_fixture);
    require(first.owned_root() != second.owned_root(),
            "Two invocations reused one private root");

    ScopedEnvironmentVariable inherited_pkgdest(
        "PKGDEST", "/tmp/shared-pkgdest-must-not-win");
    ScopedEnvironmentVariable inherited_builddir(
        "BUILDDIR", "/tmp/shared-builddir-must-not-win");
    ScopedEnvironmentVariable inherited_srcdest(
        "SRCDEST", "/tmp/shared-srcdest-must-not-win");
    SourceBuildEnvironment customization{{
        {"CFLAGS", "-O2"},
        {"CFLAGS", "-O3"},
        {"MOGUET_EMPTY", ""},
    }};
    InvocationOwnedMakepkgEnvironmentResult environment_result =
        first.make_makepkg_environment(
            customization, SourceEnvironmentEmptyValuePolicy::Forward);
    InvocationOwnedMakepkgEnvironment environment =
        take_arm<InvocationOwnedMakepkgEnvironment>(
            environment_result, "Environment projection failed");
    const auto& assignments =
        environment.source_environment().ordered_assignments;
    require(assignments.size() == customization.ordered_assignments.size() + 3,
            "Environment projection lost customization or private roots");
    require(assignments[assignments.size() - 3].key == "PKGDEST" &&
                assignments[assignments.size() - 3].value ==
                    first.pkgdest().string() &&
                assignments[assignments.size() - 2].key == "BUILDDIR" &&
                assignments[assignments.size() - 2].value ==
                    first.builddir().string() &&
                assignments[assignments.size() - 1].key == "SRCDEST" &&
                assignments[assignments.size() - 1].value ==
                    first.srcdest().string(),
            "Authority-owned roots are not the final assignments");
    require(first.owns_makepkg_environment(environment) &&
                !second.owns_makepkg_environment(environment),
            "Makepkg environment lineage crossed contexts");

    for(const std::string key : {"PKGDEST", "BUILDDIR", "SRCDEST"}) {
        InvocationOwnedMakepkgEnvironmentResult conflict =
            first.make_makepkg_environment(
                SourceBuildEnvironment{{{key, "/tmp/external"}}},
                SourceEnvironmentEmptyValuePolicy::Forward);
        const auto& failure =
            require_arm<InvocationOwnedSourceBuildContextFailure>(
                conflict, "Reserved assignment was accepted");
        require(
            failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::EnvironmentAssignmentConflict,
            "Reserved assignment returned the wrong failure");
    }
    InvocationOwnedMakepkgEnvironmentResult invalid =
        first.make_makepkg_environment(
            SourceBuildEnvironment{{{"INVALID-KEY", "value"}}},
            SourceEnvironmentEmptyValuePolicy::Forward);
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            invalid, "Invalid assignment was accepted")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::InvalidEnvironmentAssignment,
        "Invalid assignment returned the wrong failure");

    const fs::path first_root = first.owned_root();
    const fs::path second_root = second.owned_root();
    InvocationOwnedSourceBuildContext moved(std::move(first));
    require(!first.valid() && moved.valid() &&
                moved.owns_makepkg_environment(environment),
            "Context move did not transfer lineage authority");
    require_successful_cleanup(moved, first_root);
    require_successful_cleanup(second, second_root);
}

void test_editor_overlay_and_unsupported_recipe_shape_rejected() {
    PinnedBuildFixture overlay_fixture("slice3-overlay");
    InvocationOwnedSourceBuildContextResult overlay_result =
        create_invocation_owned_source_build_context(
            make_pinned_build(overlay_fixture, true));
    const auto& overlay_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            overlay_result, "Editor overlay produced a context");
    require(
        overlay_failure.reason ==
            InvocationOwnedSourceBuildContextFailureReason::EditorOverlayPresent,
        "Editor overlay returned the wrong failure");

    PinnedBuildFixture missing_srcinfo(
        "slice3-missing-srcinfo", false, false);
    InvocationOwnedSourceBuildContextResult missing_result =
        create_invocation_owned_source_build_context(
            make_pinned_build(missing_srcinfo));
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            missing_result, "Missing .SRCINFO produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::UnsupportedRecipeShape,
        "Missing .SRCINFO returned the wrong failure");

    PinnedBuildFixture symlink_fixture("slice3-symlink", true, true);
    InvocationOwnedSourceBuildContextResult symlink_result =
        create_invocation_owned_source_build_context(
            make_pinned_build(symlink_fixture));
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            symlink_result, "Tracked symlink produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
        "Tracked symlink returned the wrong failure");
}

void test_dirty_and_special_checkout_entries_fail_closed() {
    PinnedBuildFixture dirty_fixture("slice3-dirty");
    PinnedReviewedSourceBuild dirty = make_pinned_build(dirty_fixture);
    dirty_fixture.write_file("untracked-after-pin", "untracked\n");
    InvocationOwnedSourceBuildContextResult dirty_result =
        create_invocation_owned_source_build_context(std::move(dirty));
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            dirty_result, "Dirty checkout produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
        "Dirty checkout returned the wrong failure");

    PinnedBuildFixture fifo_fixture("slice3-fifo");
    PinnedReviewedSourceBuild fifo = make_pinned_build(fifo_fixture);
    const fs::path fifo_path = fifo_fixture.repository() / "untracked-fifo";
    require(::mkfifo(fifo_path.c_str(), 0600) == 0,
            "Failed to create FIFO fixture");
    InvocationOwnedSourceBuildContextResult fifo_result =
        create_invocation_owned_source_build_context(std::move(fifo));
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            fifo_result, "FIFO checkout produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
        "FIFO checkout returned the wrong failure");

    PinnedBuildFixture socket_fixture("slice3-socket");
    PinnedReviewedSourceBuild socket_pin = make_pinned_build(socket_fixture);
    const fs::path socket_path =
        socket_fixture.repository() / "untracked-socket";
    const int socket_descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(socket_descriptor >= 0, "Failed to create socket fixture");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(socket_path.string().size() < sizeof(address.sun_path),
            "Socket fixture path is too long");
    const std::string socket_path_string = socket_path.string();
    std::copy(
        socket_path_string.begin(), socket_path_string.end(),
        address.sun_path);
    require(
        ::bind(
            socket_descriptor,
            static_cast<const sockaddr*>(
                static_cast<const void*>(&address)),
            sizeof(address)) == 0,
        "Failed to bind socket fixture");
    InvocationOwnedSourceBuildContextResult socket_result =
        create_invocation_owned_source_build_context(
            std::move(socket_pin));
    static_cast<void>(::close(socket_descriptor));
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            socket_result, "Socket checkout produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
        "Socket checkout returned the wrong failure");
}

void test_path_traversal_policy() {
    require(
        invocation_owned_source_build_context_snapshot_path_is_safe_for_test(
            "PKGBUILD") &&
            invocation_owned_source_build_context_snapshot_path_is_safe_for_test(
                "local/source.conf"),
        "Valid snapshot path was rejected");
    for(const std::string path : {
            "", "/absolute", "../escape", "a/../../escape", "a//b",
            "a/./b", ".git/config", "nested/.git/config", "trailing/"}) {
        require(
            !invocation_owned_source_build_context_snapshot_path_is_safe_for_test(
                path),
            "Unsafe snapshot path was accepted: " + path);
    }
}

void test_parent_policy_and_initial_validation(
    const fs::path& default_test_parent) {
    constexpr std::uintmax_t SYNTHETIC_USER = 4242;
    constexpr std::uintmax_t DIFFERENT_USER = 4243;
    require(
        !invocation_owned_source_build_context_parent_policy_failure_for_test(
             SYNTHETIC_USER, 0700, SYNTHETIC_USER)
             .has_value(),
        "Safe effective-user-owned private parent was rejected");
    require(
        !invocation_owned_source_build_context_parent_policy_failure_for_test(
             0, 01777, SYNTHETIC_USER)
             .has_value(),
        "Safe root-owned sticky shared parent was rejected");
    require(
        invocation_owned_source_build_context_parent_policy_failure_for_test(
            0, 0777, SYNTHETIC_USER) ==
            InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
        "World-writable non-sticky parent was accepted");
    require(
        invocation_owned_source_build_context_parent_policy_failure_for_test(
            DIFFERENT_USER, 0700, SYNTHETIC_USER) ==
            InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch,
        "Unsupported parent owner was accepted");

    PinnedBuildFixture private_fixture("slice3-private-parent");
    set_invocation_owned_source_build_context_parent_path_for_test(
        default_test_parent);
    InvocationOwnedSourceBuildContext private_context =
        make_context(private_fixture);
    const fs::path private_root = private_context.owned_root();
    require(private_root.parent_path() == default_test_parent,
            "Private parent override was not the creation authority");
    require_successful_cleanup(private_context, private_root);

    TemporaryTree unsafe_tree("slice3-unsafe-parent");
    const fs::path unsafe_parent = unsafe_tree.path() / "parent";
    fs::create_directory(unsafe_parent);
    require(::chmod(unsafe_parent.c_str(), 0777) == 0,
            "Failed to create non-sticky parent fixture");
    PinnedBuildFixture unsafe_fixture("slice3-unsafe-parent-pin");
    PinnedReviewedSourceBuild unsafe_pin = make_pinned_build(unsafe_fixture);
    const std::vector<fs::path> unsafe_before =
        owned_context_root_inventory(unsafe_parent);
    set_invocation_owned_source_build_context_parent_path_for_test(
        unsafe_parent);
    InvocationOwnedSourceBuildContextResult unsafe_result =
        create_invocation_owned_source_build_context(
            std::move(unsafe_pin));
    set_invocation_owned_source_build_context_parent_path_for_test(
        default_test_parent);
    const auto& unsafe_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            unsafe_result,
            "World-writable non-sticky parent produced a context");
    require(
        unsafe_failure.stage ==
                InvocationOwnedSourceBuildContextStage::RootCreation &&
            unsafe_failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
        "Unsafe parent returned the wrong typed failure");
    require(
        owned_context_root_inventory(unsafe_parent) == unsafe_before,
        "Unsafe parent validation created a private root");

    struct stat production_parent_status{};
    require(::lstat("/tmp", &production_parent_status) == 0,
            "Failed to inspect the production context parent");
    const auto expected_production_failure =
        invocation_owned_source_build_context_parent_policy_failure_for_test(
            static_cast<std::uintmax_t>(production_parent_status.st_uid),
            static_cast<std::uintmax_t>(
                production_parent_status.st_mode & 07777),
            static_cast<std::uintmax_t>(::geteuid()));
    PinnedBuildFixture production_fixture("slice3-production-parent");
    PinnedReviewedSourceBuild production_pin =
        make_pinned_build(production_fixture);
    const std::vector<fs::path> production_before =
        owned_context_root_inventory("/tmp");
    set_invocation_owned_source_build_context_parent_path_for_test(
        std::nullopt);
    InvocationOwnedSourceBuildContextResult production_result =
        create_invocation_owned_source_build_context(
            std::move(production_pin));
    set_invocation_owned_source_build_context_parent_path_for_test(
        default_test_parent);
    if(expected_production_failure.has_value()) {
        const auto& production_failure =
            require_arm<InvocationOwnedSourceBuildContextFailure>(
                production_result,
                "Unsafe production /tmp unexpectedly produced a context");
        require(production_failure.reason == *expected_production_failure,
                "Production /tmp returned a different policy failure");
    } else {
        InvocationOwnedSourceBuildContext production_context =
            take_arm<InvocationOwnedSourceBuildContext>(
                production_result,
                "Safe production /tmp did not produce a context");
        const fs::path production_root = production_context.owned_root();
        require(production_root.parent_path() == fs::path("/tmp"),
                "Production context escaped /tmp");
        require_successful_cleanup(production_context, production_root);
    }
    require(owned_context_root_inventory("/tmp") == production_before,
            "Production parent characterization left a root");
}

void test_parent_runtime_revalidation(
    const fs::path& default_test_parent) {
    TemporaryTree mode_tree("slice3-parent-mode-runtime");
    const fs::path mode_parent = mode_tree.path() / "parent";
    fs::create_directory(mode_parent);
    require(::chmod(mode_parent.c_str(), 0700) == 0,
            "Failed to prepare safe parent mode fixture");
    PinnedBuildFixture mode_fixture("slice3-parent-mode-pin");
    set_invocation_owned_source_build_context_parent_path_for_test(
        mode_parent);
    InvocationOwnedSourceBuildContext mode_context =
        make_context(mode_fixture);
    set_invocation_owned_source_build_context_parent_path_for_test(
        default_test_parent);
    const fs::path mode_root = mode_context.owned_root();
    require(::chmod(mode_parent.c_str(), 0777) == 0,
            "Failed to make retained parent unsafe");
    InvocationOwnedSourceBuildContextValidationResult mode_validation =
        mode_context.revalidate();
    const auto& mode_validation_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            mode_validation,
            "Unsafe parent mode survived revalidation");
    require(
        mode_validation_failure.reason ==
            InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
        "Unsafe parent mode returned the wrong revalidation failure");
    InvocationOwnedMakepkgEnvironmentResult mode_environment =
        mode_context.make_makepkg_environment(
            SourceBuildEnvironment{},
            SourceEnvironmentEmptyValuePolicy::Forward);
    const auto& mode_environment_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            mode_environment,
            "Unsafe parent mode produced a makepkg environment");
    require(
        mode_environment_failure.stage ==
                InvocationOwnedSourceBuildContextStage::EnvironmentProjection &&
            mode_environment_failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
        "Environment projection lost the parent safety failure");
    InvocationOwnedSourceBuildContextCleanupResult mode_cleanup =
        mode_context.cleanup();
    const auto& mode_cleanup_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            mode_cleanup,
            "Unsafe parent mode permitted cleanup");
    require(
        mode_cleanup_failure.stage ==
                InvocationOwnedSourceBuildContextStage::Cleanup &&
            mode_cleanup_failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions &&
            mode_context.valid(),
        "Cleanup did not retain authority after parent mode drift");
    require(::chmod(mode_parent.c_str(), 0700) == 0,
            "Failed to restore safe parent mode");
    require_successful_cleanup(mode_context, mode_root);

    TemporaryTree replacement_tree("slice3-parent-replacement");
    const fs::path replacement_parent =
        replacement_tree.path() / "parent";
    const fs::path retained_parent =
        replacement_tree.path() / "retained-parent";
    fs::create_directory(replacement_parent);
    require(::chmod(replacement_parent.c_str(), 0700) == 0,
            "Failed to prepare replacement parent fixture");
    PinnedBuildFixture replacement_fixture("slice3-parent-replacement-pin");
    set_invocation_owned_source_build_context_parent_path_for_test(
        replacement_parent);
    InvocationOwnedSourceBuildContext replacement_context =
        make_context(replacement_fixture);
    set_invocation_owned_source_build_context_parent_path_for_test(
        default_test_parent);
    const fs::path replacement_root = replacement_context.owned_root();
    fs::rename(replacement_parent, retained_parent);
    fs::create_directory(replacement_parent);
    require(::chmod(replacement_parent.c_str(), 0700) == 0,
            "Failed to create replacement parent node");
    InvocationOwnedSourceBuildContextValidationResult replacement_validation =
        replacement_context.revalidate();
    const auto& replacement_validation_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            replacement_validation,
            "Replacement parent survived revalidation");
    require(
        replacement_validation_failure.reason ==
            InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
        "Replacement parent returned the wrong revalidation failure");
    InvocationOwnedMakepkgEnvironmentResult replacement_environment =
        replacement_context.make_makepkg_environment(
            SourceBuildEnvironment{},
            SourceEnvironmentEmptyValuePolicy::Forward);
    const auto& replacement_environment_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            replacement_environment,
            "Replacement parent produced a makepkg environment");
    require(
        replacement_environment_failure.reason ==
            InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
        "Environment projection lost parent replacement failure");
    InvocationOwnedSourceBuildContextCleanupResult replacement_cleanup =
        replacement_context.cleanup();
    const auto& replacement_cleanup_failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            replacement_cleanup,
            "Replacement parent permitted cleanup");
    require(
        replacement_cleanup_failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure &&
            replacement_context.valid() &&
            fs::exists(retained_parent / replacement_root.filename()),
        "Cleanup deleted or lost authority for a replaced parent");
    require(fs::remove(replacement_parent),
            "Failed to remove replacement parent node");
    fs::rename(retained_parent, replacement_parent);
    require_successful_cleanup(replacement_context, replacement_root);
}

void test_partial_construction_failure_cleanup(
    const fs::path& context_parent) {
    using Event = InvocationOwnedSourceBuildContextTestEvent;
    const std::vector<std::pair<Event, std::string>> cases = {
        {Event::AfterRootCreated, "root"},
        {Event::AfterRecipeCreated, "recipe"},
        {Event::AfterPkgdestCreated, "pkgdest"},
        {Event::AfterBuilddirCreated, "builddir"},
        {Event::AfterSrcdestCreated, "srcdest"},
    };
    for(const auto& [failure_event, label] : cases) {
        const std::vector<fs::path> parent_before =
            owned_context_root_inventory(context_parent);
        const std::vector<fs::path> production_before =
            owned_context_root_inventory("/tmp");
        PinnedBuildFixture fixture("slice3-abort-" + label);
        PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
        fs::path created_root;
        set_invocation_owned_source_build_context_test_hook(
            [failure_event, &created_root](
                Event event, const fs::path& root) {
                if(event == Event::AfterRootCreated) created_root = root;
                if(event == failure_event) {
                    throw std::runtime_error(
                        "injected construction-stage failure");
                }
            });
        InvocationOwnedSourceBuildContextResult result =
            create_invocation_owned_source_build_context(
                std::move(pin));
        set_invocation_owned_source_build_context_test_hook({});
        const auto& failure =
            require_arm<InvocationOwnedSourceBuildContextFailure>(
                result, "Injected construction failure produced a context");
        require(
            failure.stage ==
                    InvocationOwnedSourceBuildContextStage::RootCreation &&
                failure.reason ==
                    InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure &&
                !failure.construction_cleanup_failure.has_value(),
            "Construction failure or successful abort cleanup was flattened");
        require(!created_root.empty() && !fs::exists(created_root),
                "Construction abort left its private root");
        require(
            owned_context_root_inventory(context_parent) == parent_before &&
                owned_context_root_inventory("/tmp") == production_before,
            "Construction abort changed the owned root inventory");
    }
}

void test_child_pre_retain_failure_is_typed(
    const fs::path& context_parent) {
    using Event = InvocationOwnedSourceBuildContextTestEvent;
    const std::vector<fs::path> parent_before =
        owned_context_root_inventory(context_parent);
    const std::vector<fs::path> production_before =
        owned_context_root_inventory("/tmp");
    PinnedBuildFixture fixture("slice3-child-pre-retain");
    PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
    fs::path created_root;
    fs::path created_child;
    set_invocation_owned_source_build_context_test_hook(
        [&created_root, &created_child](
            Event event, const fs::path& affected_path) {
            if(event == Event::AfterRootCreated) {
                created_root = affected_path;
            }
            if(event == Event::AfterChildMkdir &&
               affected_path.filename() == "recipe") {
                created_child = affected_path;
                throw std::runtime_error(
                    "injected child pre-retain failure");
            }
        });
    InvocationOwnedSourceBuildContextResult result = [&pin]() {
        ScopedUmask known_umask(0022);
        return create_invocation_owned_source_build_context(std::move(pin));
    }();
    set_invocation_owned_source_build_context_test_hook({});
    const auto& failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            result, "Child pre-retain failure produced a context");
    require(
        failure.stage ==
                InvocationOwnedSourceBuildContextStage::RootCreation &&
            failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure &&
            failure.relative_path == fs::path("recipe") &&
            failure.diagnostic == "injected child pre-retain failure" &&
            failure.construction_cleanup_failure.has_value(),
        "Child pre-retain failure lost its typed cleanup consequence");
    const auto& cleanup_failure = *failure.construction_cleanup_failure;
    require(
        cleanup_failure.stage ==
                InvocationOwnedSourceBuildContextStage::Cleanup &&
            cleanup_failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement &&
            cleanup_failure.owned_root == created_root,
        "Child pre-retain failure lost the potential retained root");
    require(
        !created_root.empty() &&
            created_root.parent_path() == context_parent &&
            created_root.filename().string().starts_with(
                CONTEXT_ROOT_PREFIX) &&
            created_child == created_root / "recipe" &&
            directory_entry_names(created_root) ==
                std::vector<std::string>{"recipe"},
        "Child pre-retain residue escaped its exact test-owned inventory");

    // Runtime intentionally had no authority to unlink this name. The test
    // removes it only after reproving the private parent, prefix, owner, mode,
    // exact inventory, and empty child created by this fixture.
    require_owned_empty_test_directory(created_child, 0700);
    require(::rmdir(created_child.c_str()) == 0,
            "Failed to remove the proven pre-retain child residue");
    require_owned_empty_test_directory(created_root, 0700);
    require(::rmdir(created_root.c_str()) == 0,
            "Failed to remove the proven pre-retain root residue");
    require(
        owned_context_root_inventory(context_parent) == parent_before &&
            owned_context_root_inventory("/tmp") == production_before,
        "Child pre-retain characterization changed root inventory");
}

void test_child_post_retain_helper_failure_cleanup(
    const fs::path& context_parent) {
    using Event = InvocationOwnedSourceBuildContextTestEvent;
    const auto run_case = [&context_parent](
                              Event failure_event,
                              std::string child_leaf,
                              std::string label) {
        const std::vector<fs::path> parent_before =
            owned_context_root_inventory(context_parent);
        const std::vector<fs::path> production_before =
            owned_context_root_inventory("/tmp");
        PinnedBuildFixture fixture("slice3-child-post-retain-" + label);
        PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
        fs::path created_root;
        fs::path affected_child;
        std::optional<ScopedUmask> restrictive_umask;
        set_invocation_owned_source_build_context_test_hook(
            [failure_event, child_leaf, &created_root, &affected_child,
             &restrictive_umask](
                Event event, const fs::path& affected_path) {
                if(event == Event::BeforePrivateRootCreation) {
                    restrictive_umask.emplace(0777);
                }
                if(event == Event::AfterRootCreated) {
                    created_root = affected_path;
                }
                if(event == failure_event &&
                   affected_path.filename() == child_leaf) {
                    affected_child = affected_path;
                    throw std::runtime_error(
                        "injected child post-retain failure");
                }
            });
        InvocationOwnedSourceBuildContextResult result =
            create_invocation_owned_source_build_context(std::move(pin));
        set_invocation_owned_source_build_context_test_hook({});
        restrictive_umask.reset();
        const auto& failure =
            require_arm<InvocationOwnedSourceBuildContextFailure>(
                result, "Child post-retain failure produced a context");
        if(failure.stage !=
               InvocationOwnedSourceBuildContextStage::RootCreation ||
           failure.reason !=
               InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure ||
           failure.relative_path != fs::path(child_leaf) ||
           failure.diagnostic != "injected child post-retain failure" ||
           failure.construction_cleanup_failure.has_value()) {
            std::ostringstream message;
            message << "Post-retain child cleanup changed the primary typed failure: case="
                    << label << " stage=" << static_cast<int>(failure.stage)
                    << " reason=" << static_cast<int>(failure.reason)
                    << " path=" << failure.relative_path.string()
                    << " cleanup_failure="
                    << failure.construction_cleanup_failure.has_value();
            if(failure.diagnostic.has_value()) {
                message << " diagnostic=" << *failure.diagnostic;
            }
            if(failure.construction_cleanup_failure.has_value()) {
                message << " cleanup_reason="
                        << static_cast<int>(
                               failure.construction_cleanup_failure->reason)
                        << " cleanup_path="
                        << failure.construction_cleanup_failure->relative_path
                               .string();
            }
            throw std::runtime_error(message.str());
        }
        require(
            !created_root.empty() &&
                affected_child == created_root / child_leaf &&
                !fs::exists(created_root),
            "Post-retain child cleanup left a partial context root");
        require(
            owned_context_root_inventory(context_parent) == parent_before &&
                owned_context_root_inventory("/tmp") == production_before,
            "Post-retain child cleanup changed root inventory");
    };

    const std::vector<std::pair<Event, std::string>> recipe_phases = {
        {Event::AfterChildRetained, "retained"},
        {Event::AfterChildModeSealed, "mode-sealed"},
        {Event::BeforeChildFinalOpen, "final-open"},
        {Event::BeforeChildFinalReproof, "final-reproof"},
    };
    for(const auto& [event, label] : recipe_phases) {
        run_case(event, "recipe", "recipe-" + label);
    }
    for(const std::string child : {"pkgdest", "build", "srcdest"}) {
        run_case(
            Event::AfterChildRetained, child,
            child + "-retained");
    }
}

void test_provisional_child_cleanup_refuses_unproven_content(
    const fs::path& context_parent) {
    using Event = InvocationOwnedSourceBuildContextTestEvent;
    const std::vector<fs::path> production_before =
        owned_context_root_inventory("/tmp");

    {
        const std::vector<fs::path> parent_before =
            owned_context_root_inventory(context_parent);
        PinnedBuildFixture fixture("slice3-child-replacement");
        PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
        fs::path created_root;
        fs::path replaced_child;
        fs::path retained_child;
        std::optional<ScopedUmask> known_umask;
        set_invocation_owned_source_build_context_test_hook(
            [&created_root, &replaced_child, &retained_child, &known_umask](
                Event event, const fs::path& affected_path) {
                if(event == Event::BeforePrivateRootCreation) {
                    known_umask.emplace(0022);
                }
                if(event == Event::AfterRootCreated) {
                    created_root = affected_path;
                }
                if(event != Event::AfterChildRetained ||
                   affected_path.filename() != "recipe") {
                    return;
                }
                replaced_child = affected_path;
                retained_child =
                    affected_path.parent_path() / "retained-recipe";
                fs::rename(replaced_child, retained_child);
                require(
                    ::symlink(
                        "retained-recipe", replaced_child.c_str()) == 0,
                    "Failed to install child symlink replacement");
                throw std::runtime_error(
                    "injected replaced child failure");
            });
        InvocationOwnedSourceBuildContextResult result =
            create_invocation_owned_source_build_context(std::move(pin));
        set_invocation_owned_source_build_context_test_hook({});
        known_umask.reset();
        const auto& failure =
            require_arm<InvocationOwnedSourceBuildContextFailure>(
                result, "Replaced provisional child produced a context");
        require(
            failure.reason ==
                    InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure &&
                failure.diagnostic ==
                    "injected replaced child failure" &&
                failure.construction_cleanup_failure.has_value(),
            "Replaced provisional child lost its cleanup consequence");
        require(
            created_root.parent_path() == context_parent &&
                created_root.filename().string().starts_with(
                    CONTEXT_ROOT_PREFIX),
            "Replaced-child root escaped the test-owned parent");
        struct stat replacement_status{};
        require(
            ::lstat(replaced_child.c_str(), &replacement_status) == 0 &&
                S_ISLNK(replacement_status.st_mode) &&
                replacement_status.st_uid == ::geteuid() &&
                fs::read_symlink(replaced_child) ==
                    fs::path("retained-recipe") &&
                directory_entry_names(created_root) ==
                    std::vector<std::string>{"recipe", "retained-recipe"},
            "Provisional cleanup followed or deleted a child replacement");
        require_owned_empty_test_directory(retained_child, 0700);

        require(::unlink(replaced_child.c_str()) == 0,
                "Failed to remove the proven child symlink replacement");
        require(::rmdir(retained_child.c_str()) == 0,
                "Failed to remove the proven retained child");
        require_owned_empty_test_directory(created_root, 0700);
        require(::rmdir(created_root.c_str()) == 0,
                "Failed to remove the proven replaced-child root");
        require(
            owned_context_root_inventory(context_parent) == parent_before,
            "Replaced-child characterization changed root inventory");
    }

    {
        const std::vector<fs::path> parent_before =
            owned_context_root_inventory(context_parent);
        PinnedBuildFixture fixture("slice3-child-foreign-content");
        PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
        fs::path created_root;
        fs::path created_child;
        fs::path unexpected_sibling;
        fs::path foreign_entry;
        std::optional<ScopedUmask> known_umask;
        set_invocation_owned_source_build_context_test_hook(
            [&created_root, &created_child, &unexpected_sibling,
             &foreign_entry, &known_umask](
                Event event, const fs::path& affected_path) {
                if(event == Event::BeforePrivateRootCreation) {
                    known_umask.emplace(0022);
                }
                if(event == Event::AfterRootCreated) {
                    created_root = affected_path;
                }
                if(event != Event::AfterChildRetained ||
                   affected_path.filename() != "recipe") {
                    return;
                }
                created_child = affected_path;
                unexpected_sibling =
                    affected_path.parent_path() / "unexpected-sibling";
                foreign_entry = affected_path / "foreign-entry";
                fs::create_directory(unexpected_sibling);
                require(::chmod(unexpected_sibling.c_str(), 0700) == 0,
                        "Failed to seal unexpected sibling fixture");
                std::ofstream output(foreign_entry);
                output << "foreign\n";
                output.close();
                require(static_cast<bool>(output),
                        "Failed to write foreign child content");
                throw std::runtime_error(
                    "injected nonempty child failure");
            });
        InvocationOwnedSourceBuildContextResult result =
            create_invocation_owned_source_build_context(std::move(pin));
        set_invocation_owned_source_build_context_test_hook({});
        known_umask.reset();
        const auto& failure =
            require_arm<InvocationOwnedSourceBuildContextFailure>(
                result, "Nonempty provisional child produced a context");
        require(
            failure.reason ==
                    InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure &&
                failure.diagnostic == "injected nonempty child failure" &&
                failure.construction_cleanup_failure.has_value(),
            "Nonempty provisional child lost its cleanup consequence");
        require(
            created_root.parent_path() == context_parent &&
                created_root.filename().string().starts_with(
                    CONTEXT_ROOT_PREFIX),
            "Foreign-content root escaped the test-owned parent");
        struct stat foreign_status{};
        require(
            directory_entry_names(created_root) ==
                    std::vector<std::string>{"recipe", "unexpected-sibling"} &&
                directory_entry_names(created_child) ==
                    std::vector<std::string>{"foreign-entry"} &&
                ::lstat(foreign_entry.c_str(), &foreign_status) == 0 &&
                S_ISREG(foreign_status.st_mode) &&
                foreign_status.st_uid == ::geteuid() &&
                static_cast<mode_t>(foreign_status.st_mode & 07777) == 0644 &&
                foreign_status.st_nlink == 1,
            "Provisional cleanup removed foreign content or an unexpected sibling");
        require_owned_test_directory(created_root, 0700);
        require_owned_test_directory(created_child, 0700);
        require_owned_empty_test_directory(unexpected_sibling, 0700);

        require(::unlink(foreign_entry.c_str()) == 0,
                "Failed to remove the proven foreign child content");
        require(::rmdir(created_child.c_str()) == 0,
                "Failed to remove the proven nonempty child");
        require(::rmdir(unexpected_sibling.c_str()) == 0,
                "Failed to remove the proven unexpected sibling");
        require_owned_empty_test_directory(created_root, 0700);
        require(::rmdir(created_root.c_str()) == 0,
                "Failed to remove the proven foreign-content root");
        require(
            owned_context_root_inventory(context_parent) == parent_before,
            "Foreign-content characterization changed root inventory");
    }

    require(owned_context_root_inventory("/tmp") == production_before,
            "Provisional cleanup safety tests changed production inventory");
}

void test_construction_cleanup_failure_is_retained(
    const fs::path& context_parent) {
    using Event = InvocationOwnedSourceBuildContextTestEvent;
    const std::vector<fs::path> parent_before =
        owned_context_root_inventory(context_parent);
    const std::vector<fs::path> production_before =
        owned_context_root_inventory("/tmp");
    PinnedBuildFixture fixture("slice3-abort-cleanup-failure");
    PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
    fs::path created_root;
    set_invocation_owned_source_build_context_test_hook(
        [&created_root](Event event, const fs::path& root) {
            if(event == Event::AfterRootCreated) created_root = root;
            if(event == Event::AfterRecipeCreated) {
                throw std::runtime_error(
                    "injected primary construction failure");
            }
            if(event == Event::BeforeCleanup) {
                throw std::runtime_error(
                    "injected construction abort cleanup failure");
            }
        });
    InvocationOwnedSourceBuildContextResult result =
        create_invocation_owned_source_build_context(std::move(pin));
    set_invocation_owned_source_build_context_test_hook({});
    const auto& failure =
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            result, "Dual construction failure produced a context");
    require(
        failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure &&
            failure.diagnostic ==
                "injected primary construction failure" &&
            failure.construction_cleanup_failure.has_value(),
        "Primary construction failure lost its cleanup consequence");
    const auto& cleanup_failure = *failure.construction_cleanup_failure;
    require(
        cleanup_failure.stage ==
                InvocationOwnedSourceBuildContextStage::Cleanup &&
            cleanup_failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure &&
            cleanup_failure.diagnostic ==
                "injected construction abort cleanup failure" &&
            cleanup_failure.owned_root == created_root,
        "Nested construction cleanup failure lost typed evidence");

    require(created_root.parent_path() == context_parent &&
                created_root.filename().string().starts_with(
                    CONTEXT_ROOT_PREFIX),
            "Retained abort root escaped the test-owned parent");
    require_owned_empty_test_directory(created_root / "recipe", 0700);
    require(::rmdir((created_root / "recipe").c_str()) == 0,
            "Failed to remove the proven partial recipe root");
    require_owned_empty_test_directory(created_root, 0700);
    require(::rmdir(created_root.c_str()) == 0,
            "Failed to remove the proven partial context root");
    require(
        owned_context_root_inventory(context_parent) == parent_before &&
            owned_context_root_inventory("/tmp") == production_before,
        "Dual failure characterization left a context root");
}

void test_context_creation_does_not_change_umask() {
    for(const mode_t mask : {static_cast<mode_t>(0022),
                             static_cast<mode_t>(0777)}) {
        PinnedBuildFixture fixture(
            "slice3-umask-" + std::to_string(mask));
        PinnedReviewedSourceBuild pin = make_pinned_build(fixture);
        const mode_t original = read_process_umask();
        std::optional<ScopedUmask> injected_umask;
        bool retained_requested_umask = false;
        set_invocation_owned_source_build_context_test_hook(
            [mask, &injected_umask, &retained_requested_umask](
                InvocationOwnedSourceBuildContextTestEvent event,
                const fs::path&) {
                if(event == InvocationOwnedSourceBuildContextTestEvent::
                                BeforePrivateRootCreation) {
                    injected_umask.emplace(mask);
                }
                if(event == InvocationOwnedSourceBuildContextTestEvent::
                                BeforeFinalReviewedSourceReproof) {
                    retained_requested_umask =
                        read_process_umask() == mask;
                    injected_umask.reset();
                }
            });
        {
            InvocationOwnedSourceBuildContextResult result =
                create_invocation_owned_source_build_context(
                    std::move(pin));
            set_invocation_owned_source_build_context_test_hook({});
            if(const auto* failure =
                   std::get_if<InvocationOwnedSourceBuildContextFailure>(
                       &result)) {
                std::ostringstream message;
                message << "Restrictive caller umask prevented context creation: stage="
                        << static_cast<int>(failure->stage)
                        << " reason=" << static_cast<int>(failure->reason)
                        << " path=" << failure->relative_path.string();
                if(failure->system_error.has_value()) {
                    message << " errno=" << failure->system_error->value();
                }
                if(failure->diagnostic.has_value()) {
                    message << " diagnostic=" << *failure->diagnostic;
                }
                throw std::runtime_error(message.str());
            }
            InvocationOwnedSourceBuildContext context =
                take_arm<InvocationOwnedSourceBuildContext>(
                    result,
                    "Restrictive caller umask prevented context creation");
            require(retained_requested_umask &&
                        read_process_umask() == original,
                    "Context creation changed the caller umask");
            const fs::path root = context.owned_root();
            require(path_mode(root) == 0700 &&
                        path_mode(context.recipe_root()) == 0500 &&
                        path_mode(context.pkgdest()) == 0700 &&
                        path_mode(context.builddir()) == 0700 &&
                        path_mode(context.srcdest()) == 0700,
                    "Context modes depend on the caller umask");
            require_successful_cleanup(context, root);
        }
        injected_umask.reset();
        require(read_process_umask() == original,
                "Umask fixture did not restore the caller policy");
    }
}

void test_source_race_and_unsafe_root_fail_closed() {
    PinnedBuildFixture race_fixture("slice3-race");
    PinnedReviewedSourceBuild race_pin = make_pinned_build(race_fixture);
    fs::path race_root;
    set_invocation_owned_source_build_context_test_hook(
        [&race_fixture, &race_root](
            InvocationOwnedSourceBuildContextTestEvent event,
            const fs::path& root) {
            if(event == InvocationOwnedSourceBuildContextTestEvent::
                            AfterPrivateRootsCreated) {
                race_root = root;
            }
            if(event == InvocationOwnedSourceBuildContextTestEvent::
                            BeforeFinalReviewedSourceReproof) {
                race_fixture.write_file(
                    "PKGBUILD", "concurrent source replacement\n");
            }
        });
    InvocationOwnedSourceBuildContextResult race_result =
        create_invocation_owned_source_build_context(std::move(race_pin));
    set_invocation_owned_source_build_context_test_hook({});
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            race_result, "Source race produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
        "Source race returned the wrong failure");
    require(!race_root.empty() && !fs::exists(race_root),
            "Failed source race left a private root");

    PinnedBuildFixture mode_fixture("slice3-mode");
    PinnedReviewedSourceBuild mode_pin = make_pinned_build(mode_fixture);
    fs::path unsafe_root;
    set_invocation_owned_source_build_context_test_hook(
        [&unsafe_root](
            InvocationOwnedSourceBuildContextTestEvent event,
            const fs::path& root) {
            if(event != InvocationOwnedSourceBuildContextTestEvent::
                            AfterPrivateRootsCreated) {
                return;
            }
            unsafe_root = root;
            if(::chmod(root.c_str(), 0755) != 0) {
                throw std::runtime_error("Failed to alter root mode");
            }
        });
    InvocationOwnedSourceBuildContextResult mode_result =
        create_invocation_owned_source_build_context(std::move(mode_pin));
    set_invocation_owned_source_build_context_test_hook({});
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            mode_result, "Unsafe root mode produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
        "Unsafe root mode returned the wrong failure");
    require(!unsafe_root.empty() && !fs::exists(unsafe_root),
            "Unsafe root mode left a private root");
}

void test_makepkg_unavailable_and_cleanup_failure_are_typed() {
    PinnedBuildFixture executable_fixture("slice3-executable");
    PinnedReviewedSourceBuild executable_pin =
        make_pinned_build(executable_fixture);
    bool root_was_created = false;
    set_invocation_owned_source_build_context_test_hook(
        [&root_was_created](
            InvocationOwnedSourceBuildContextTestEvent event,
            const fs::path&) {
            if(event == InvocationOwnedSourceBuildContextTestEvent::
                            AfterPrivateRootsCreated) {
                root_was_created = true;
            }
        });
    set_invocation_owned_source_build_context_makepkg_path_for_test(
        fs::path("/tmp/moguet-missing-makepkg-for-slice3"));
    InvocationOwnedSourceBuildContextResult unavailable =
        create_invocation_owned_source_build_context(
            std::move(executable_pin));
    set_invocation_owned_source_build_context_makepkg_path_for_test(
        std::nullopt);
    set_invocation_owned_source_build_context_test_hook({});
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            unavailable, "Missing makepkg produced a context")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable,
        "Missing makepkg returned the wrong failure");
    require(!root_was_created,
            "Missing makepkg fell through to private-root creation");

    PinnedBuildFixture cleanup_fixture("slice3-cleanup");
    InvocationOwnedSourceBuildContext context = make_context(cleanup_fixture);
    const fs::path root = context.owned_root();
    {
        std::ofstream output(context.builddir() / "ordinary-output");
        output << "build output\n";
    }
    require(
        ::symlink(
            "ordinary-output",
            (context.builddir() / "output-link").c_str()) == 0,
        "Failed to create cleanup symlink fixture");
    require(
        ::mkfifo((context.srcdest() / "source-fifo").c_str(), 0600) == 0,
        "Failed to create cleanup FIFO fixture");
    fs::create_directory(context.builddir() / "sealed-directory");
    require(
        ::chmod(
            (context.builddir() / "sealed-directory").c_str(), 0000) == 0,
        "Failed to seal cleanup directory fixture");
    set_invocation_owned_source_build_context_test_hook(
        [](InvocationOwnedSourceBuildContextTestEvent event,
           const fs::path&) {
            if(event == InvocationOwnedSourceBuildContextTestEvent::
                            BeforeCleanup) {
                throw std::runtime_error("injected cleanup failure");
            }
        });
    InvocationOwnedSourceBuildContextCleanupResult failed_cleanup =
        context.cleanup();
    set_invocation_owned_source_build_context_test_hook({});
    require(
        require_arm<InvocationOwnedSourceBuildContextFailure>(
            failed_cleanup, "Injected cleanup failure was hidden")
                .reason ==
            InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
        "Injected cleanup returned the wrong failure");
    require(context.valid() && fs::exists(root),
            "Cleanup failure discarded retry authority");
    require_successful_cleanup(context, root);
}

void test_reviewed_binding_does_not_rebind() {
    PinnedBuildFixture fixture("slice3-binding");
    InvocationOwnedSourceBuildContext context = make_context(fixture);
    const fs::path root = context.owned_root();
    const ReviewedSourceStateRecordBinding retained =
        context.reviewed_binding();
    ReviewedSourceStateStoreReadResult current_result =
        read_reviewed_source_state(retained.package_base());
    const auto& current = require_arm<ReviewedSourceStateStoreRead>(
        current_result, "Current #411 state could not be read");
    require(current.observed.has_value(),
            "Current #411 state lacks a record");
    ReviewedSourceStateStorePublishResult advanced =
        publish_reviewed_source_state(
            ReviewedSourceState::make(
                retained.package_base(),
                SourceRevisionIdentity::git_commit(fixture.second_oid())),
            current.observed);
    require(
        std::holds_alternative<ReviewedSourceStateStorePublished>(advanced),
        "Failed to advance #411 fixture state");
    ReviewedSourceStateStoreReadResult advanced_result =
        read_reviewed_source_state(retained.package_base());
    const auto& advanced_read = require_arm<ReviewedSourceStateStoreRead>(
        advanced_result, "Advanced #411 state could not be read");
    require(context.reviewed_binding() == retained,
            "Existing context rebound to current #411 state");
    require(
        std::holds_alternative<ReviewedSourceStateRecordBindingMismatch>(
            compare_reviewed_source_state_record_binding(
                retained, advanced_read)),
        "Advanced #411 state still matched the creation-time binding");
    require_successful_cleanup(context, root);
}

} // namespace

int main() {
    try {
        TemporaryTree context_parent("slice3-owned-context-parent");
        require(::chmod(context_parent.path().c_str(), 0700) == 0,
                "Failed to prepare invocation context test parent");
        set_invocation_owned_source_build_context_parent_path_for_test(
            context_parent.path());
        test_exact_snapshot_and_physical_separation();
        test_environment_precedence_unique_roots_and_move_only_lineage();
        test_editor_overlay_and_unsupported_recipe_shape_rejected();
        test_dirty_and_special_checkout_entries_fail_closed();
        test_path_traversal_policy();
        test_parent_policy_and_initial_validation(context_parent.path());
        test_parent_runtime_revalidation(context_parent.path());
        test_partial_construction_failure_cleanup(context_parent.path());
        test_child_pre_retain_failure_is_typed(context_parent.path());
        test_child_post_retain_helper_failure_cleanup(
            context_parent.path());
        test_provisional_child_cleanup_refuses_unproven_content(
            context_parent.path());
        test_construction_cleanup_failure_is_retained(
            context_parent.path());
        test_context_creation_does_not_change_umask();
        test_source_race_and_unsafe_root_fail_closed();
        test_makepkg_unavailable_and_cleanup_failure_are_typed();
        test_reviewed_binding_does_not_rebind();
        require(owned_context_root_inventory(context_parent.path()).empty(),
                "Focused test left an invocation-owned context root");
        set_invocation_owned_source_build_context_parent_path_for_test(
            std::nullopt);
        std::cout << "invocation-owned source-build context tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        set_invocation_owned_source_build_context_test_hook({});
        set_invocation_owned_source_build_context_makepkg_path_for_test(
            std::nullopt);
        set_invocation_owned_source_build_context_parent_path_for_test(
            std::nullopt);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
