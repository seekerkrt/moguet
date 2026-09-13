#include "invocation_owned_recipe_acquisition.hpp"

#include "aur_devel_update.hpp"
#include "aur_rpc.hpp"
#include "invocation_owned_source_build_context.hpp"
#include "reviewed_source_acceptance.hpp"
#include "reviewed_source_presentation.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <curl/curl.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

// RPC/HTTP are observation seams only. The factory below is the real typed
// bootstrap observer, with an isolated installed DB and real local P/R reads.
CurlGlobal::CurlGlobal() {
    if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl init");
}
CurlGlobal::~CurlGlobal() {
    curl_global_cleanup();
}
std::map<std::string, AurPackageInfo> AurClient::info_many(const std::vector<std::string>&) {
    throw std::runtime_error("unexpected RPC");
}
std::optional<AurPackageInfo> AurClient::info_strict(const std::string&) {
    throw std::runtime_error("unexpected RPC");
}

namespace {
namespace fs = std::filesystem;
using Owner = InvocationOwnedRecipeAcquisition;
using Stage = RecipeAcquisitionStage;
using Reason = RecipeAcquisitionFailureReason;

static_assert(!std::is_default_constructible_v<Owner>);
static_assert(!std::is_copy_constructible_v<Owner>);
static_assert(std::is_nothrow_move_constructible_v<Owner>);
static_assert(!std::is_move_assignable_v<Owner>);
static_assert(std::is_nothrow_destructible_v<Owner>);
static_assert(!std::is_constructible_v<Owner, fs::path, std::string, SourceRevisionIdentity>);
static_assert(!std::is_invocable_v<decltype(acquire_invocation_owned_recipe), AurReviewedSourceReviewIdentity>);
static_assert(!std::is_invocable_v<decltype(acquire_invocation_owned_recipe), fs::path, std::string, std::string>);
static_assert(!std::is_invocable_v<decltype(create_invocation_owned_source_build_context), Owner>);
static_assert(!std::is_default_constructible_v<DevelTrackingBootstrapTrial>);
static_assert(!std::is_constructible_v<Owner, std::nullptr_t>);
static_assert(!std::is_constructible_v<DevelTrackingBootstrapTrial, PackageChildIdentity, SourceRevisionIdentity, std::string, InstalledArtifactBinding, ReviewedSourceStateStoreRead>);

void require(bool value, const std::string& message) {
    if(!value) throw std::runtime_error(message);
}
template <class T, class V>
T take(V&& value) {
    auto* arm = std::get_if<T>(&value);
    if(!arm) {
        if constexpr(std::is_same_v<std::remove_cvref_t<V>, RecipeAcquisitionResult>) {
            const auto& e = std::get<RecipeAcquisitionFailure>(value);
            throw std::runtime_error("acquisition stage=" + std::to_string(static_cast<int>(e.stage)) + " reason=" +
                                     std::to_string(static_cast<int>(e.reason)) + " errno=" + std::to_string(e.error_number.value_or(0)) +
                                     (e.process ? " process=" + std::to_string(e.process->outcome.index()) + " output=" + e.process->output : ""));
        }
        throw std::runtime_error("unexpected variant arm");
    }
    return std::move(*arm);
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    require(!output.fail(), "fixture write failed");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture read failed");
    return {std::istreambuf_iterator<char>(input), {}};
}
struct Environment {
    std::string name;
    std::optional<std::string> previous;
    Environment(std::string key, const std::string& value) : name(std::move(key)) {
        if(const char* old = ::getenv(name.c_str())) previous = old;
        require(::setenv(name.c_str(), value.c_str(), 1) == 0, "setenv failed");
    }
    ~Environment() {
        if(previous)
            ::setenv(name.c_str(), previous->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

struct Fixture {
    fs::path root;
    fs::path remote;
    std::vector<std::unique_ptr<Environment>> environment;
    std::string metadata = "pkgbase = acquisition\n\tpkgver = 1\n\tpkgrel = 1\n\tarch = any\n\tsource = git+https://example.invalid/upstream.git\n\tsource = fix.patch\npkgname = acquisition-git\n";
    PackageBaseIdentity base = PackageBaseIdentity::make(PackageSourceIdentity::aur(
                                                             SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/acquisition.git")),
                                                         "acquisition");
    PackageChildIdentity child = PackageChildIdentity::make(base, "acquisition-git");
    std::string x;
    std::string y;
    RecipeAcquisitionTestHooks hooks;
    unsigned fetches = 0;

    explicit Fixture(bool sha256 = false) {
        char pattern[] = "/tmp/moguet-recipe-fixture-XXXXXX";
        const auto* path = ::mkdtemp(pattern);
        require(path, "mkdtemp failed");
        root = path;
        remote = root / "remote";
        for(const auto* name : {"home", "cache", "state", "remote", "private", "s3"})
            fs::create_directory(root / name);
        for(const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
                {"HOME", (root / "home").string()}, {"XDG_CACHE_HOME", (root / "cache").string()}, {"XDG_STATE_HOME", (root / "state").string()}})
            environment.push_back(std::make_unique<Environment>(key, value));
        git({"init", "-q", "--template=", "-b", "main", sha256 ? "--object-format=sha256" : "--object-format=sha1"});
        write(remote / "PKGBUILD", "pkgname=acquisition-git\npkgver=1\npkgrel=1\narch=(any)\nsource=('git+https://example.invalid/upstream.git' fix.patch)\n");
        write(remote / ".SRCINFO", metadata);
        write(remote / "fix.patch", "exact-X-patch\n");
        x = commit();
        const auto record = root / "db/local/acquisition-git-1-1";
        fs::create_directories(record);
        write(root / "db/local/ALPM_DB_VERSION", "9\n");
        write(record / "desc", "%NAME%\nacquisition-git\n\n%BASE%\nacquisition\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
        write(record / "files", "%FILES%\nusr/share/acquisition\n\n");
        write(record / "mtree", "raw-mtree");
        InstalledRecordObservationTestHooks installed;
        installed.database_path = (root / "db").string();
        installed.expected_owner = ::geteuid();
        installed.statfs = [](int, struct statfs* value) { *value = {}; value->f_type = 0xef53; return 0; };
        installed.name_to_handle = [](int, const char*, struct file_handle* handle, int* mount, int) {
            *mount = 1;
            if(handle->handle_bytes == 0) {
                handle->handle_bytes = 1;
                errno = EOVERFLOW;
                return -1;
            }
            handle->handle_type = 1;
            handle->f_handle[0] = 42;
            return 0;
        };
        set_installed_record_observation_test_hooks(installed);
        set_aur_devel_update_database_paths_for_test(PacmanDatabasePaths{"/", root / "db"});
        hooks.parent = root / "private";
        hooks.process = [this](const auto& invocation, const auto& policy) { return transport(invocation, policy); };
        set_recipe_acquisition_test_hooks(hooks);
        set_invocation_owned_source_build_context_parent_path_for_test(root / "s3");
    }
    ~Fixture() {
        set_recipe_acquisition_test_hooks({});
        set_invocation_owned_source_build_context_parent_path_for_test(std::nullopt);
        set_devel_tracking_bootstrap_test_hooks({});
        set_aur_devel_update_database_paths_for_test(std::nullopt);
        set_installed_record_observation_test_hooks({});
        std::error_code error;
        fs::remove_all(root, error);
    }
    std::string git(std::vector<std::string> args) {
        args.insert(args.begin(), {"-C", remote.string()});
        const auto result = capture_explicit_process_output_raw({"/usr/bin/git", std::move(args), {"PATH=/usr/bin:/bin", "LC_ALL=C", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null", "GIT_AUTHOR_NAME=Fixture", "GIT_AUTHOR_EMAIL=fixture@example.invalid", "GIT_COMMITTER_NAME=Fixture", "GIT_COMMITTER_EMAIL=fixture@example.invalid"}}, true);
        require(result.exit_code == 0, "fixture git failed");
        auto output = result.output;
        if(!output.empty() && output.back() == '\n') output.pop_back();
        return output;
    }
    std::string commit() {
        git({"add", "-A"});
        git({"commit", "-q", "-m", "fixture"});
        return git({"rev-parse", "HEAD"});
    }
    std::shared_ptr<const DevelTrackingBootstrapTrial> trial(const std::string& oid = {}) {
        const std::string expected = oid.empty() ? x : oid;
        DevelTrackingBootstrapTestHooks observation;
        observation.recipe_head = [expected](const auto&, const auto&) { return BoundedCapturedProcessResult{expected + "\tHEAD\n", BoundedProcessExited{0}}; };
        observation.recipe_metadata = [this, expected](const std::string& url) -> std::optional<std::string> {
            require(url.ends_with("&id=" + expected), "cgit did not use exact observed id");
            return metadata;
        };
        set_devel_tracking_bootstrap_test_hooks(std::move(observation));
        return take<std::shared_ptr<const DevelTrackingBootstrapTrial>>(observe_devel_tracking_bootstrap(child));
    }
    BoundedCapturedProcessResult transport(ExplicitProcessInvocation invocation, const BoundedProcessPolicy& policy) {
        // The canonical endpoint is inspected BEFORE the offline-only transport
        // substitution. Neither file transport nor a URL override exists in production.
        for(const auto* key : {"GIT_DIR=", "GIT_WORK_TREE=", "GIT_INDEX_FILE=", "GIT_CONFIG_COUNT=", "GIT_OBJECT_DIRECTORY=",
                               "GIT_ALTERNATE_OBJECT_DIRECTORIES=", "GIT_TEMPLATE_DIR=", "HOME="}) {
            for(const auto& value : invocation.environment)
                require(!value.starts_with(key), "inherited Git contamination");
        }
        require(std::find(invocation.arguments.begin(), invocation.arguments.end(), "protocol.http.allow=never") != invocation.arguments.end(), "HTTPS restriction absent");
        auto fetch = std::find(invocation.arguments.begin(), invocation.arguments.end(), "fetch");
        if(fetch != invocation.arguments.end()) {
            ++fetches;
            require(invocation.arguments[invocation.arguments.size() - 2] == "https://aur.archlinux.org/acquisition.git", "canonical endpoint changed");
            invocation.arguments[invocation.arguments.size() - 2] = remote.string();
            const auto file_policy = std::find(invocation.arguments.begin(), invocation.arguments.end(), "protocol.file.allow=never");
            require(file_policy != invocation.arguments.end(), "file transport restriction absent");
            *file_policy = "protocol.file.allow=always";
        }
        return capture_bounded_explicit_process_output_raw(invocation, policy);
    }
};

PresentedReviewedSourceTarget review(Owner& owner) {
    auto preflight = take<ReviewedSourceFatalStatePreflight>(preflight_reviewed_source_fatal_state(owner.identity().package_base()));
    auto requirement = take<ReviewedSourceReviewRequirement>(plan_reviewed_source_lifecycle_from_preflight(
        owner.identity(), std::move(preflight), ReviewedSourceReviewPurpose::DevelTrackingBootstrap));
    auto projection = take<TrustedAurReviewedSourceProjection>(trusted_git_project_aur_reviewed_source(owner.checkout(), owner.identity(), std::nullopt));
    auto materialized = take<TrustedAurReviewedSourceReview>(trusted_git_materialize_aur_reviewed_source_review(owner.checkout(), std::move(projection)));
    auto bound = take<ReviewedSourceVerifiedLifecycleTarget>(bind_reviewed_source_verified_review(std::move(requirement), std::move(materialized)));
    std::ostringstream output;
    auto presented = take<PresentedReviewedSourceTarget>(present_reviewed_source_target(std::move(bound), output));
    require(output.str().find("exact-X-patch") != std::string::npos, "full review did not present exact X bytes");
    return presented;
}
PinnedReviewedSourceBuild pin(Owner& owner) {
    auto presented = review(owner);
    auto yes = take<ExplicitConfirmationAcceptance>(parse_explicit_confirmation_input("yes"));
    auto accepted = take<AcceptedReviewedSourceTarget>(decide_reviewed_source_acceptance(std::move(presented), std::move(yes)));
    auto materialized = take<AcceptedReviewedSourceCheckout>(materialize_accepted_reviewed_source_checkout(std::move(accepted), owner.checkout()));
    return take<PinnedReviewedSourceBuild>(publish_accepted_reviewed_source_checkout(std::move(materialized)));
}

void positives() {
    for(bool sha256 : {false, true}) {
        Fixture f(sha256);
        const auto trial = f.trial(); // X observed while remote still at X
        write(f.remote / "fix.patch", "advanced-Y-patch\n");
        f.y = f.commit();
        require(f.x != f.y, "remote did not advance");
        Owner a = take<Owner>(acquire_invocation_owned_recipe(*trial));
        Owner b = take<Owner>(acquire_invocation_owned_recipe(*trial));
        require(f.fetches == 2, "acquisition retried or reobserved remote");
        require(a.workspace_path() != b.workspace_path() && a.checkout().inode() != b.checkout().inode(), "two owners share workspace");
        require(!fs::exists(a.checkout().path() / "PKGBUILD") && !fs::exists(a.checkout().path() / ".git/FETCH_HEAD"), "acquisition performed checkout or used FETCH_HEAD");
        require(a.identity().target_revision() == SourceRevisionIdentity::git_commit(f.x), "remote Y replaced expected X");
        const auto b_path = b.workspace_path();
        require(!b.cleanup() && !fs::exists(b_path) && fs::exists(a.workspace_path()), "B cleanup affected A");
        const auto acquired_path = a.workspace_path();
        Owner retained(std::move(a));
        auto pinned = pin(retained);
        require(read(retained.checkout().path() / "fix.patch") == "exact-X-patch\n", "pin materialized Y");
        auto context = take<InvocationOwnedSourceBuildContext>(create_invocation_owned_source_build_context(std::move(pinned)));
        require(read(context.recipe_root() / "fix.patch") == "exact-X-patch\n", "S3 source differs from exact review");
        require(fs::exists(acquired_path), "owner died before S3 reproof");
        require(!retained.cleanup() && !fs::exists(acquired_path), "explicit cleanup failed");
        require(read(context.recipe_root() / "fix.patch") == "exact-X-patch\n", "S3 depends on acquisition after copy");
        auto cleanup = context.cleanup();
        require(!std::holds_alternative<InvocationOwnedSourceBuildContextFailure>(cleanup), "S3 cleanup failed");
        require(fs::is_empty(f.root / "cache"), "acquisition touched old cache");
        std::cout << "exact X / remote advance / full review / pin / S3 / two owners " << (sha256 ? "sha256" : "sha1") << " PASS\n";
    }
}

// Snapshot names, entry kinds and all regular bytes, including packed refs and
// objects. Timestamps are deliberately not part of this external-write oracle.
std::map<fs::path, std::pair<fs::file_type, std::string>> repository_snapshot(const fs::path& root) {
    std::map<fs::path, std::pair<fs::file_type, std::string>> result;
    for(const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto type = entry.symlink_status().type();
        require(type == fs::file_type::directory || type == fs::file_type::regular, "unexpected external fixture entry");
        result.emplace(entry.path().lexically_relative(root),
                       std::pair{type, type == fs::file_type::regular ? read(entry.path()) : std::string{}});
    }
    return result;
}

void preexisting_git_rejected_before_init() {
    for(const std::string mode : {"gitfile", "directory", "symlink", "dangling-symlink", "fifo", "socket"}) {
        Fixture f;
        const auto trial = f.trial();
        const auto external = f.root / "external.git";
        f.git({"clone", "--bare", "--no-local", "--quiet", f.remote.string(), external.string()});
        const std::string git_dir = "--git-dir=" + external.string();
        require(f.git({git_dir, "config", "--local", "core.bare"}) == "true", "external fixture is not bare");
        const auto config_before = read(external / "config");
        const auto head_before = read(external / "HEAD");
        const auto symbolic_before = f.git({git_dir, "symbolic-ref", "HEAD"});
        const auto refs_before = f.git({git_dir, "show-ref"});
        const auto objects_before = f.git({git_dir, "cat-file", "--batch-all-objects", "--batch-check"});
        require(!refs_before.empty() && !objects_before.empty(), "external inventory is empty");
        const auto snapshot_before = repository_snapshot(external);
        fs::path created;
        unsigned process_calls = 0;
        unsigned init_calls = 0;
        unsigned cleanup_calls = 0;
        f.hooks.process = [&](const ExplicitProcessInvocation& invocation, const BoundedProcessPolicy& policy) {
            ++process_calls;
            if(std::find(invocation.arguments.begin(), invocation.arguments.end(), "init") != invocation.arguments.end()) ++init_calls;
            return f.transport(invocation, policy);
        };
        f.hooks.event = [&](Stage stage, const fs::path& root) {
            if(stage == Stage::Initialization) {
                created = root;
                const auto git = root / "moguet/acquisition/.git";
                if(mode == "gitfile") {
                    write(git, "gitdir: " + external.string() + "\n");
                    require(::chmod(git.c_str(), 0644) == 0, "gitfile chmod failed");
                    struct stat entry{};
                    require(::lstat(git.c_str(), &entry) == 0 && S_ISREG(entry.st_mode) &&
                                (entry.st_mode & 07777) == 0644 && entry.st_nlink == 1,
                            "F1 gitfile shape differs from counterexample");
                }
                if(mode == "directory") fs::create_directory(git);
                if(mode == "symlink") fs::create_directory_symlink(external, git);
                if(mode == "dangling-symlink") fs::create_symlink(f.root / "absent", git);
                if(mode == "fifo") require(::mkfifo(git.c_str(), 0600) == 0, "fixture FIFO failed");
                if(mode == "socket") require(::mknod(git.c_str(), S_IFSOCK | 0600, 0) == 0, "fixture socket failed");
            }
            if(stage == Stage::Cleanup) {
                ++cleanup_calls;
                require(root == created, "cleanup target is not the self-owned root");
                require(repository_snapshot(external) == snapshot_before, "external repository changed before cleanup");
            }
        };
        set_recipe_acquisition_test_hooks(f.hooks);
        const auto failure = take<RecipeAcquisitionFailure>(acquire_invocation_owned_recipe(*trial));
        require(init_calls == 0 && process_calls == 0, mode + " started a Git child before rejection");
        require(failure.stage == Stage::Initialization && failure.reason == Reason::UnsafeFilesystem && !failure.process,
                mode + " lost pre-init typed filesystem failure");
        require(cleanup_calls == 1, "abort cleanup was skipped or retried");
        require(read(external / "config") == config_before && read(external / "HEAD") == head_before,
                "external config or HEAD bytes changed");
        require(repository_snapshot(external) == snapshot_before, "cleanup changed external repository entries or bytes");
        require(f.git({git_dir, "symbolic-ref", "HEAD"}) == symbolic_before && f.git({git_dir, "show-ref"}) == refs_before &&
                    f.git({git_dir, "cat-file", "--batch-all-objects", "--batch-check"}) == objects_before,
                "external HEAD / refs / objects changed");
        if(mode == "gitfile" || mode == "directory") {
            require(!failure.cleanup && !failure.abandoned_root && !fs::exists(created), "safe abort cleanup left owned root");
        } else {
            // Existing cleanup refuses special entries rather than following
            // them. Preserve its typed residue consequence; fixture teardown
            // below is separate from acquisition cleanup authority.
            require(failure.cleanup && failure.cleanup->reason == Reason::UnsafeFilesystem &&
                        failure.abandoned_root == created && fs::exists(created),
                    "unsafe cleanup consequence lost");
        }
        std::cout << "pre-init " << mode << " / Git child 0 / external config HEAD refs objects unchanged / cleanup PASS\n";
    }
}

void failures() {
    for(const std::string mode : {"missing-fetch", "missing-object", "tag", "blob", "format", "metadata", "nonzero", "timeout", "signal", "cancel-zero",
                                  "launch", "setup", "io", "capture", "entries", "bytes", "oversized-metadata", "collision", "parent-symlink", "unsafe-parent",
                                  "root-replacement", "leaf-replacement", "git-symlink", "gitfile", "git-replacement", "config", "alternates", "http-alternates", "commondir", "shallow", "promisor", "hardlink", "root-mode", "permission", "tree-metadata-size", "large-object"}) {
        Fixture f;
        std::string expected = f.x;
        if(mode == "missing-fetch" || mode == "missing-object") expected = std::string(40, 'a');
        if(mode == "tag") {
            f.git({"tag", "-a", "fixture", "-m", "tag"});
            expected = f.git({"rev-parse", "refs/tags/fixture"});
        }
        if(mode == "blob") expected = f.git({"rev-parse", f.x + ":PKGBUILD"});
        if(mode == "metadata") f.metadata += "\n";
        if(mode == "tree-metadata-size") {
            write(f.remote / ".SRCINFO", std::string(256 * 1024 + 1, 'x'));
            expected = f.commit();
        }
        const auto trial = f.trial(expected);
        fs::path created;
        auto original_transport = f.hooks.process;
        f.hooks.process = [&](const ExplicitProcessInvocation& call, const BoundedProcessPolicy& original_policy) {
            const bool fetch = std::find(call.arguments.begin(), call.arguments.end(), "fetch") != call.arguments.end();
            if(mode == "large-object" && std::find(call.arguments.begin(), call.arguments.end(), "--batch-all-objects") != call.arguments.end())
                return BoundedCapturedProcessResult{expected + " commit 33554433\n", BoundedProcessExited{0}};
            if(mode == "format" && std::find(call.arguments.begin(), call.arguments.end(), "--show-object-format=storage") != call.arguments.end())
                return BoundedCapturedProcessResult{"sha256\n", BoundedProcessExited{0}};
            if(fetch) {
                if(mode == "missing-object") return BoundedCapturedProcessResult{{}, BoundedProcessExited{0}};
                if(mode == "io") return BoundedCapturedProcessResult{{}, BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::Poll, EIO}};
                auto invocation = call;
                auto policy = original_policy;
                if(mode == "launch") invocation.executable = "/no-such-acquisition-git";
                if(mode == "setup") invocation.working_directory_fd = -1;
                if(mode == "nonzero" || mode == "timeout" || mode == "signal" || mode == "cancel-zero" || mode == "capture") {
                    invocation.executable = "/bin/sh";
                    std::string script = "exit 23";
                    if(mode == "timeout") {
                        script = "sleep 5";
                        policy.hard_timeout = std::chrono::milliseconds(30);
                    }
                    if(mode == "signal") script = "kill -TERM $$";
                    if(mode == "cancel-zero") script = "trap 'exit 0' INT; kill -INT $PPID; while :; do :; done";
                    if(mode == "capture") {
                        script = "printf 'overflow' >&2";
                        policy.stdout_capture_limit = 4;
                    }
                    invocation.arguments = {"-c", script};
                }
                if(mode == "launch" || mode == "setup" || mode == "nonzero" || mode == "timeout" || mode == "signal" || mode == "cancel-zero" || mode == "capture")
                    return capture_bounded_explicit_process_output_raw(invocation, policy);
            }
            return original_transport(call, original_policy);
        };
        if(mode == "entries") f.hooks.entry_limit = 1;
        if(mode == "bytes") f.hooks.byte_limit = 1024;
        if(mode == "collision") {
            f.hooks.random_suffix = [] { return std::string(32, 'a'); };
            fs::create_directory(*f.hooks.parent / ("moguet-recipe-acquisition-" + std::string(32, 'a')));
        }
        if(mode == "parent-symlink") {
            fs::create_directory_symlink(f.root / "private", f.root / "link");
            f.hooks.parent = f.root / "link";
        }
        if(mode == "unsafe-parent") ::chmod((f.root / "private").c_str(), 0777);
        if(mode == "permission") ::chmod((f.root / "private").c_str(), 0500);
        f.hooks.event = [&](Stage stage, const fs::path& root) {
            if(stage == Stage::WorkspaceCreation) created = root;
            if(stage != Stage::Fetch) return;
            const auto repo = root / "moguet/acquisition";
            if(mode == "bytes") write(repo / "large", std::string(4096, 'x'));
            if(mode == "oversized-metadata") write(repo / ".git/config", std::string(9000, 'x'));
            if(mode == "root-mode") ::chmod(root.c_str(), 0755);
            if(mode == "root-replacement") {
                fs::rename(root, f.root / "saved");
                fs::create_directory_symlink(f.root / "saved", root);
            }
            if(mode == "leaf-replacement") {
                fs::rename(repo, f.root / "saved");
                fs::create_directory_symlink(f.root / "saved", repo);
            }
            if(mode == "git-symlink" || mode == "gitfile") {
                fs::rename(repo / ".git", f.root / "saved");
                if(mode == "gitfile")
                    write(repo / ".git", "gitdir: " + (f.root / "saved").string() + "\n");
                else
                    fs::create_directory_symlink(f.root / "saved", repo / ".git");
            }
            if(mode == "git-replacement") {
                fs::rename(repo / ".git", f.root / "saved");
                fs::copy(f.root / "saved", repo / ".git", fs::copy_options::recursive);
            }
            if(mode == "shallow") write(repo / ".git/shallow", f.x + "\n");
            if(mode == "promisor") write(repo / ".git/objects/pack/evil.promisor", "");
            if(mode == "commondir") write(repo / ".git/commondir", f.remote.string());
            if(mode == "http-alternates") write(repo / ".git/objects/info/http-alternates", "https://example.invalid/objects");
            if(mode == "config") {
                const auto path = repo / ".git/config";
                write(path, read(path) + "[core]\n hooksPath=/evil\n");
            }
            if(mode == "alternates") write(repo / ".git/objects/info/alternates", f.remote.string());
            if(mode == "hardlink") fs::create_hard_link(repo / ".git/config", repo / "link");
        };
        set_recipe_acquisition_test_hooks(f.hooks);
        const auto result = acquire_invocation_owned_recipe(*trial);
        const auto* failure = std::get_if<RecipeAcquisitionFailure>(&result);
        require(failure, mode + " acquired partial success");
        if(!created.empty() && !failure->cleanup) require(!fs::exists(created), "successful abort cleanup left workspace");
        if(mode == "tag" || mode == "blob") require(failure->reason == Reason::WrongObjectType, mode + " wrong object taxonomy");
        if(mode == "missing-object") require(failure->reason == Reason::ExpectedCommitUnavailable, "missing object lost");
        if(mode == "metadata") require(failure->reason == Reason::MetadataMismatch, "metadata mismatch lost");
        if(mode == "format") require(failure->reason == Reason::ObjectFormatMismatch, "format mismatch lost");
        if(mode == "missing-fetch") require(failure->stage == Stage::Fetch && failure->reason == Reason::GitProcessFailed, "fetch failure became object absence");
        if(mode == "cancel-zero") require(failure->reason == Reason::Cancelled && failure->process && failure->process->cancellation_signal == SIGINT &&
                                              std::get<BoundedProcessExited>(failure->process->outcome).exit_code == 0,
                                          "cancel plus exit zero became success");
        if(mode == "timeout") require(failure->process && std::holds_alternative<BoundedProcessTimedOut>(failure->process->outcome), "timeout lost");
        if(mode == "signal") require(failure->process && std::holds_alternative<BoundedProcessSignaled>(failure->process->outcome), "signal lost");
        if(mode == "launch" || mode == "setup") require(failure->process && std::holds_alternative<BoundedProcessLaunchOrSetupFailure>(failure->process->outcome), "setup detail lost");
        if(mode == "capture") require(failure->process && std::holds_alternative<BoundedProcessCaptureLimitExceeded>(failure->process->outcome), "capture limit lost");
        if(mode == "entries" || mode == "bytes" || mode == "oversized-metadata" || mode == "tree-metadata-size" || mode == "large-object") require(failure->reason == Reason::ResourceLimit, "resource bound lost");
        if(mode == "root-replacement" || mode == "leaf-replacement" || mode == "root-mode" || mode == "hardlink") require(failure->cleanup.has_value(), "unsafe cleanup became success");
        if(mode == "collision") require(fs::exists(*f.hooks.parent / ("moguet-recipe-acquisition-" + std::string(32, 'a'))), "preexisting collision deleted");
        if(mode == "permission") {
            require(failure->error_number == EACCES, "permission errno lost");
            ::chmod((f.root / "private").c_str(), 0700);
        }
        require(fs::is_empty(f.root / "cache"), "failure touched old cache");
        std::cout << mode << " fail-closed PASS\n";
    }
}

void cleanup_and_environment() {
    for(const std::string mode : {"destructor", "exception", "decline", "cancel", "s3-failure", "cleanup-replacement", "cleanup-ancestor-replacement", "primary-cleanup", "environment", "submodule-no-execution", "fixed-parent"}) {
        Fixture f;
        if(mode == "submodule-no-execution") {
            write(f.remote / ".gitmodules", "[submodule \"evil\"]\n path=evil\n url=ssh://example.invalid/evil\n");
            f.x = f.commit();
        }
        const auto trial = f.trial();
        if(mode == "fixed-parent") {
            f.hooks.parent.reset();
            set_recipe_acquisition_test_hooks(f.hooks);
        }
        fs::path owned_path;
        if(mode == "environment") {
            const auto marker = f.root / "hook-marker";
            const auto hook = f.root / "home/hook";
            write(hook, "#!/bin/sh\ntouch '" + marker.string() + "'\n");
            ::chmod(hook.c_str(), 0700);
            fs::create_directories(f.root / "template/hooks");
            fs::copy_file(hook, f.root / "template/hooks/post-checkout");
            write(f.root / "home/.gitconfig", "[init]\n templateDir=" + (f.root / "template").string() + "\n[core]\n hooksPath=" + (f.root / "template/hooks").string() + "\n");
            for(const auto* key : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_TEMPLATE_DIR", "GIT_CONFIG_SYSTEM"})
                f.environment.push_back(std::make_unique<Environment>(key, (f.root / "template").string()));
        }
        {
            auto owner = take<Owner>(acquire_invocation_owned_recipe(*trial));
            owned_path = owner.workspace_path();
            if(mode == "fixed-parent") require(owned_path.parent_path() == "/tmp", "fixed parent policy changed");
            if(mode == "decline" || mode == "cancel") {
                auto shown = review(owner);
                ExplicitConfirmationResult response = mode == "decline" ? ExplicitConfirmationResult{take<ConfirmationDeclined>(parse_explicit_confirmation_input("no"))}
                                                                        : ExplicitConfirmationResult{ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput}};
                auto decision = decide_reviewed_source_acceptance(std::move(shown), std::move(response));
                require(!std::holds_alternative<AcceptedReviewedSourceTarget>(decision), "decline/cancel accepted");
            }
            if(mode == "s3-failure") {
                auto pinned = pin(owner);
                write(owner.checkout().path() / "PKGBUILD", "drift\n");
                require(std::holds_alternative<InvocationOwnedSourceBuildContextFailure>(create_invocation_owned_source_build_context(std::move(pinned))), "S3 ignored drift");
            }
            if(mode == "cleanup-replacement") {
                f.hooks.event = [&](Stage stage, const fs::path& path) { if(stage == Stage::Cleanup) { fs::rename(path, f.root / "saved"); fs::create_directory_symlink(f.root / "saved", path); } };
                set_recipe_acquisition_test_hooks(f.hooks);
                require(owner.cleanup().has_value() && fs::exists(f.root / "saved/moguet/acquisition/.git"), "cleanup removed replacement");
            }
            if(mode == "submodule-no-execution") require(!fs::exists(owner.checkout().path() / ".git/modules") && !fs::exists(owner.checkout().path() / "evil"), "acquisition executed submodule");
            if(mode == "cleanup-ancestor-replacement") {
                bool replaced = false;
                f.hooks.before_remove = [&](const fs::path& path) {
                    if(replaced) return;
                    replaced = true;
                    const auto git = path / "moguet/acquisition/.git";
                    fs::rename(git, f.root / "saved");
                    fs::create_directory(git);
                    for(const auto& entry : fs::directory_iterator(f.root / "saved"))
                        fs::rename(entry.path(), git / entry.path().filename());
                };
                set_recipe_acquisition_test_hooks(f.hooks);
                require(owner.cleanup().has_value() && fs::exists(owned_path / "moguet/acquisition/.git/config"), "cleanup removed a child under replacement ancestor");
            }
            if(mode == "exception") {
                try {
                    Owner moved(std::move(owner));
                    throw std::runtime_error("fixture exception");
                } catch(const std::runtime_error&) {
                }
            }
            if(mode == "environment") {
                auto pinned = pin(owner);
                require(!fs::exists(f.root / "hook-marker"), "untrusted hook ran");
            }
        }
        if(mode != "cleanup-replacement" && mode != "cleanup-ancestor-replacement") require(!fs::exists(owned_path), "destructor left safe workspace");
        if(mode == "primary-cleanup") {
            f.hooks.process = [&](const ExplicitProcessInvocation& invocation, const BoundedProcessPolicy& policy) {
                if(std::find(invocation.arguments.begin(), invocation.arguments.end(), "fetch") != invocation.arguments.end()) return BoundedCapturedProcessResult{{}, BoundedProcessTimedOut{}};
                return f.transport(invocation, policy);
            };
            f.hooks.event = [](Stage stage, const fs::path& path) { if(stage == Stage::Cleanup) ::chmod(path.c_str(), 0000); };
            set_recipe_acquisition_test_hooks(f.hooks);
            const auto failure = take<RecipeAcquisitionFailure>(acquire_invocation_owned_recipe(*trial));
            require(failure.stage == Stage::Fetch && failure.process && std::holds_alternative<BoundedProcessTimedOut>(failure.process->outcome) && failure.cleanup && failure.abandoned_root && fs::exists(*failure.abandoned_root),
                    "cleanup consequence replaced primary timeout");
            for(const auto& entry : fs::directory_iterator(f.root / "private"))
                ::chmod(entry.path().c_str(), 0700);
        }
        std::cout << mode << " lifetime / cleanup PASS\n";
    }
}
} // namespace

int main() {
    try {
        preexisting_git_rejected_before_init();
        positives();
        failures();
        cleanup_and_environment();
        std::cout << "invocation-owned recipe acquisition PASS\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
