#include "invocation_owned_recipe_acquisition.hpp"

#include "persistent_checkout.hpp"
#include "logging.hpp"
#include "trusted_git_process_policy.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <map>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Stage = RecipeAcquisitionStage;
using Reason = RecipeAcquisitionFailureReason;

enum class GitInitializationState {
    BeforeGitInit,
    AfterGitInit,
};

// Recipe history budgets, independent of S4 source/build limits. Verification
// happens after fetch: these are NOT hard transport-byte or disk quotas.
constexpr std::size_t MAX_ENTRIES = 32768;
constexpr std::size_t MAX_DEPTH = 64;
constexpr std::uintmax_t MAX_BYTES = 256 * 1024 * 1024;
constexpr std::uintmax_t MAX_OBJECT_BYTES = 32 * 1024 * 1024;
constexpr std::size_t MAX_METADATA_BYTES = 256 * 1024;
constexpr std::size_t MAX_CONFIG_BYTES = 8192;
constexpr std::size_t MAX_OBJECT_STREAM = MAX_ENTRIES * 96;
constexpr auto ACQUISITION_TIMEOUT = std::chrono::seconds(90);
constexpr auto CLEANUP_TIMEOUT = std::chrono::seconds(5);

#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
RecipeAcquisitionTestHooks g_hooks;
#endif

void notify(Stage stage, const fs::path& path) {
#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
    if(g_hooks.event) g_hooks.event(stage, path);
#else
    static_cast<void>(stage);
    static_cast<void>(path);
#endif
}

class Failure final : public std::exception {
public:
    RecipeAcquisitionFailure detail;
    explicit Failure(RecipeAcquisitionFailure value) : detail(std::move(value)) {
    }
    const char* what() const noexcept override {
        return "recipe acquisition failed";
    }
};

[[noreturn]] void fail(Stage stage, Reason reason, std::optional<int> error = std::nullopt) {
    throw Failure({stage, reason, error});
}

class Descriptor final {
public:
    explicit Descriptor(int fd = -1) noexcept : fd_(fd) {
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {
    }
    Descriptor& operator=(Descriptor&& other) noexcept {
        if(fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        return *this;
    }
    ~Descriptor() noexcept {
        if(fd_ >= 0) ::close(fd_);
    }
    int get() const noexcept {
        return fd_;
    }

private:
    int fd_;
};

Descriptor open_beneath(int parent, const fs::path& name, int flags, Stage stage, bool same_device = true) {
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    if(same_device) how.resolve |= RESOLVE_NO_XDEV;
    const int fd = static_cast<int>(::syscall(SYS_openat2, parent, name.c_str(), &how, sizeof(how)));
    if(fd < 0) fail(stage, errno == ELOOP || errno == EXDEV ? Reason::UnsafeFilesystem : Reason::IoFailure, errno);
    return Descriptor(fd);
}

struct stat status(int fd, Stage stage) {
    struct stat value{};
    if(::fstat(fd, &value) != 0) fail(stage, Reason::IoFailure, errno);
    return value;
}

bool same_identity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_uid == b.st_uid && a.st_mode == b.st_mode &&
           (!S_ISREG(a.st_mode) || a.st_nlink == b.st_nlink);
}

void require_named(int parent, const fs::path& name, const struct stat& expected, Stage stage) {
    struct stat named{};
    if(::fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) fail(stage, Reason::IdentityChanged, errno);
    if(!same_identity(expected, named)) fail(stage, Reason::IdentityChanged);
}

void require_private(const struct stat& value, dev_t device, Stage stage) {
    if(!S_ISDIR(value.st_mode) || value.st_uid != ::geteuid() ||
       (value.st_mode & 07777) != 0700 || value.st_dev != device) fail(stage, Reason::UnsafeFilesystem);
}

std::string random_suffix() {
#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
    if(g_hooks.random_suffix) return g_hooks.random_suffix();
#endif
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, GRND_NONBLOCK);
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0) fail(Stage::WorkspaceCreation, Reason::CreationFailed, errno);
        offset += static_cast<std::size_t>(count);
    }
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    for(const auto byte : bytes) {
        result += HEX[byte >> 4];
        result += HEX[byte & 15];
    }
    return result;
}

struct DirectoryCloser {
    void operator()(DIR* directory) const noexcept {
        ::closedir(directory);
    }
};

struct Inventory {
    fs::path path;
    struct stat identity;
};

struct Budget {
    Clock::time_point deadline;
    std::size_t entries = 0;
    std::uintmax_t bytes = 0;
    std::size_t entry_limit = MAX_ENTRIES;
    std::uintmax_t byte_limit = MAX_BYTES;
    explicit Budget(Clock::time_point end) : deadline(end) {
#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
        if(g_hooks.entry_limit) entry_limit = *g_hooks.entry_limit;
        if(g_hooks.byte_limit) byte_limit = *g_hooks.byte_limit;
#endif
    }
    void check(Stage stage) const {
        if(Clock::now() >= deadline) fail(stage, Reason::ResourceLimit);
    }
};

// Root-relative NO_XDEV/NO_SYMLINKS opens and post-open identity comparison
// cover the Git DB as well as its config/refs. No worktree-content authority is
// minted here; reviewed byte/PKGBUILD rules remain with full review and S3.
void inventory(int root, int directory, const fs::path& prefix, dev_t device,
               std::size_t depth, Budget& budget, Stage stage, std::vector<Inventory>& result) {
    budget.check(stage);
    if(depth > MAX_DEPTH) fail(stage, Reason::ResourceLimit);
    auto scan = open_beneath(directory, ".", O_RDONLY | O_DIRECTORY, stage);
    const int scan_fd = ::fcntl(scan.get(), F_DUPFD_CLOEXEC, 3);
    if(scan_fd < 0) fail(stage, Reason::IoFailure, errno);
    std::unique_ptr<DIR, DirectoryCloser> stream(::fdopendir(scan_fd));
    if(!stream) {
        ::close(scan_fd);
        fail(stage, Reason::IoFailure, errno);
    }
    while(true) {
        budget.check(stage);
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if(!entry) {
            if(errno != 0) fail(stage, Reason::IoFailure, errno);
            break;
        }
        const std::string name(entry->d_name);
        if(name == "." || name == "..") continue;
        if(++budget.entries > budget.entry_limit) fail(stage, Reason::ResourceLimit);
        struct stat named{};
        if(::fstatat(directory, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) fail(stage, Reason::IoFailure, errno);
        if((!S_ISREG(named.st_mode) && !S_ISDIR(named.st_mode)) || named.st_uid != ::geteuid() ||
           named.st_dev != device || (named.st_mode & 0022) != 0 ||
           (S_ISREG(named.st_mode) && named.st_nlink != 1)) fail(stage, Reason::UnsafeFilesystem);
        if(named.st_size < 0) fail(stage, Reason::UnsafeFilesystem);
        const auto bytes = S_ISREG(named.st_mode) ? static_cast<std::uintmax_t>(named.st_size) : 0;
        if(bytes > budget.byte_limit - budget.bytes) fail(stage, Reason::ResourceLimit);
        budget.bytes += bytes;
        const fs::path relative = prefix / name;
        auto opened = open_beneath(root, relative, O_RDONLY | (S_ISDIR(named.st_mode) ? O_DIRECTORY : 0), stage);
        if(!same_identity(named, status(opened.get(), stage))) fail(stage, Reason::IdentityChanged);
        require_named(directory, name, named, stage);
        if(S_ISDIR(named.st_mode)) inventory(root, opened.get(), relative, device, depth + 1, budget, stage, result);
        result.push_back({relative, named}); // children precede parents for removal
    }
}

std::string read_regular(int root, const fs::path& path, std::size_t limit, Stage stage) {
    auto fd = open_beneath(root, path, O_RDONLY, stage);
    const auto before = status(fd.get(), stage);
    if(!S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_uid != ::geteuid() ||
       (before.st_mode & 0022) != 0 || before.st_size < 0) fail(stage, Reason::UnsafeFilesystem);
    if(static_cast<std::uintmax_t>(before.st_size) > limit) fail(stage, Reason::ResourceLimit);
    std::string bytes;
    std::array<char, 4096> buffer{};
    while(true) {
        const auto count = ::read(fd.get(), buffer.data(), buffer.size());
        if(count < 0 && errno == EINTR) continue;
        if(count < 0) fail(stage, Reason::IoFailure, errno);
        if(count == 0) break;
        if(static_cast<std::size_t>(count) > limit - bytes.size()) fail(stage, Reason::ResourceLimit);
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    require_named(root, path, before, stage);
    const auto after = status(fd.get(), stage);
    if(!same_identity(before, after) || before.st_size != after.st_size || bytes.size() != static_cast<std::size_t>(after.st_size))
        fail(stage, Reason::IdentityChanged);
    return bytes;
}

std::uintmax_t parse_size(std::string_view text, Stage stage) {
    std::uintmax_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if(error != std::errc{} || end != text.data() + text.size() || text.empty()) fail(stage, Reason::MalformedRepository);
    return value;
}
} // namespace

struct InvocationOwnedRecipeAcquisition::State {
    AurReviewedSourceReviewIdentity expected;
    fs::path parent_path = "/tmp";
    fs::path root_path;
    std::string leaf;
    Descriptor filesystem_root;
    Descriptor parent;
    Descriptor root;
    Descriptor repository;
    struct stat parent_identity{};
    struct stat root_identity{};
    struct stat repository_identity{};
    std::optional<struct stat> git_identity;
    std::optional<ValidatedCacheRoot> cache;
    std::optional<ValidatedCachePath> checkout;
    std::string config_bytes;
    bool has_created_root = false;
    bool cleanup_attempted = false;
    RecipeAcquisitionCleanupResult cleanup_result;
    Clock::time_point deadline = Clock::now() + ACQUISITION_TIMEOUT;
    Stage active_stage = Stage::WorkspaceCreation;

    explicit State(const DevelTrackingBootstrapTrial& trial)
        : expected(AurReviewedSourceReviewIdentity::make(trial.package().package_base(), trial.recipe_revision())) {
    }

    ~State() noexcept {
        static_cast<void>(cleanup());
    }

    void require_lineage(Stage stage) const {
        auto named_parent = open_beneath(filesystem_root.get(), parent_path.relative_path(), O_RDONLY | O_DIRECTORY, stage, false);
        if(!same_identity(parent_identity, status(named_parent.get(), stage))) fail(stage, Reason::IdentityChanged);
        if(!same_identity(parent_identity, status(parent.get(), stage))) fail(stage, Reason::IdentityChanged);
        require_named(parent.get(), leaf, root_identity, stage);
        if(!same_identity(root_identity, status(root.get(), stage))) fail(stage, Reason::IdentityChanged);
        require_private(root_identity, parent_identity.st_dev, stage);
        if(cache) cache->require_unchanged_identity();
        if(checkout) {
            static_cast<void>(revalidate_trusted_cache_path(*checkout, CachePathRequirement::ExistingDirectory));
            require_named(root.get(), fs::path("moguet") / expected.package_base().package_base(), repository_identity, stage);
            if(!same_identity(repository_identity, status(repository.get(), stage))) fail(stage, Reason::IdentityChanged);
        }
    }

    void create() {
#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
        if(g_hooks.parent) parent_path = *g_hooks.parent;
#endif
        filesystem_root = Descriptor(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if(filesystem_root.get() < 0) fail(Stage::WorkspaceCreation, Reason::IoFailure, errno);
        if(!parent_path.is_absolute()) fail(Stage::WorkspaceCreation, Reason::UnsafeFilesystem);
        parent = open_beneath(filesystem_root.get(), parent_path.relative_path(), O_RDONLY | O_DIRECTORY, Stage::WorkspaceCreation, false);
        parent_identity = status(parent.get(), Stage::WorkspaceCreation);
        if((parent_identity.st_uid != 0 && parent_identity.st_uid != ::geteuid()) ||
           ((parent_identity.st_mode & 0022) != 0 && (parent_identity.st_mode & S_ISVTX) == 0))
            fail(Stage::WorkspaceCreation, Reason::UnsafeFilesystem);
        for(unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto suffix = random_suffix();
            if(suffix.size() != 32 || suffix.find_first_not_of("0123456789abcdef") != std::string::npos)
                fail(Stage::WorkspaceCreation, Reason::CreationFailed);
            leaf = "moguet-recipe-acquisition-" + suffix;
            root_path = parent_path / leaf;
            require_named(filesystem_root.get(), parent_path.relative_path(), parent_identity, Stage::WorkspaceCreation);
            if(::mkdirat(parent.get(), leaf.c_str(), 0700) == 0) {
                has_created_root = true;
                break;
            }
            if(errno != EEXIST) fail(Stage::WorkspaceCreation, Reason::CreationFailed, errno);
        }
        if(!has_created_root) fail(Stage::WorkspaceCreation, Reason::CreationFailed, EEXIST);
        // If retention fails, cleanup cannot prove creation continuity and
        // reports residue rather than unlinking by an unproven random name.
        root = open_beneath(parent.get(), leaf, O_RDONLY | O_DIRECTORY, Stage::WorkspaceCreation);
        root_identity = status(root.get(), Stage::WorkspaceCreation);
        require_private(root_identity, parent_identity.st_dev, Stage::WorkspaceCreation);
        require_lineage(Stage::WorkspaceCreation);
        notify(Stage::WorkspaceCreation, root_path);
        require_lineage(Stage::WorkspaceCreation);
        xdg_paths::EnvironmentSnapshot environment;
        environment.xdg_cache_home = root_path.string();
        const auto paths = xdg_paths::resolve_cache(environment);
        cache.emplace(adopt_trusted_cache_root(paths, xdg_directory_safety::prepare_directory(paths)));
        require_lineage(Stage::WorkspaceCreation);
        checkout.emplace(create_trusted_cache_directory(*cache, expected.package_base().package_base()));
        repository = open_beneath(root.get(), fs::path("moguet") / expected.package_base().package_base(), O_RDONLY | O_DIRECTORY, Stage::WorkspaceCreation);
        repository_identity = status(repository.get(), Stage::WorkspaceCreation);
        require_private(repository_identity, root_identity.st_dev, Stage::WorkspaceCreation);
        require_lineage(Stage::WorkspaceCreation);
    }

    void inspect(Stage stage, GitInitializationState git_state) {
        active_stage = stage;
        require_lineage(stage);
        if(git_state == GitInitializationState::BeforeGitInit) {
            // Git init resolves regular gitfiles even with --git-dir=.git.
            // Reject every preexisting entry before any Git child can write
            // outside this fresh checkout. This check is not an atomic sandbox
            // against same-UID replacement between inspection and execution.
            struct stat git_entry{};
            if(::fstatat(repository.get(), ".git", &git_entry, AT_SYMLINK_NOFOLLOW) == 0)
                fail(stage, Reason::UnsafeFilesystem);
            if(errno != ENOENT) fail(stage, Reason::IoFailure, errno);
        }
        Budget budget(deadline);
        std::vector<Inventory> nodes;
        inventory(root.get(), root.get(), {}, root_identity.st_dev, 0, budget, stage, nodes);
        if(git_state == GitInitializationState::AfterGitInit) {
            if(git_identity) require_named(repository.get(), ".git", *git_identity, stage);
            std::size_t metadata_steps = 0;
            require_safe_persistent_checkout_git_metadata(*checkout, [&] {
                budget.check(stage);
                if(++metadata_steps > MAX_ENTRIES * 8 + 256) fail(stage, Reason::ResourceLimit);
            });
            for(const auto& node : nodes) {
                const auto name = node.path.filename().string();
                if(name == "shallow" || name.ends_with(".promisor") || name == "http-alternates" || name == "alternates")
                    fail(stage, Reason::MalformedRepository);
            }
            if(!config_bytes.empty() && read_regular(repository.get(), ".git/config", MAX_CONFIG_BYTES, stage) != config_bytes)
                fail(stage, Reason::MalformedRepository);
        }
        require_lineage(stage);
    }

    BoundedCapturedProcessResult run(Stage stage, std::initializer_list<std::string> operation,
                                     std::size_t limit, GitInitializationState git_state = GitInitializationState::AfterGitInit, bool diagnostic = false) {
        notify(stage, root_path);
        inspect(stage, git_state);
        auto arguments = trusted_git_recipe_acquisition_process_arguments();
        arguments.push_back("--git-dir=.git");
        arguments.push_back("--work-tree=.");
        arguments.insert(arguments.end(), operation.begin(), operation.end());
        Descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if(input.get() < 0) fail(stage, Reason::IoFailure, errno);
        ExplicitProcessInvocation invocation{"/usr/bin/git", std::move(arguments),
                                             trusted_git_process_environment(TrustedGitProcessEnvironmentMode::ManagedOperation)};
        invocation.working_directory_fd = repository.get();
        invocation.standard_input_fd = input.get();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if(remaining.count() <= 0) fail(stage, Reason::ResourceLimit);
        const BoundedProcessPolicy policy{remaining, std::chrono::milliseconds(200), limit, true, diagnostic};
        if(stage == Stage::Fetch) Logger::raw_cmd("git fetch --no-tags --no-recurse-submodules --no-auto-maintenance --no-write-fetch-head " +
                                                  expected.canonical_git_remote() + " " + *expected.target_revision().git_commit());
        auto result = [&] {
#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
            if(g_hooks.process) return g_hooks.process(invocation, policy);
#endif
            return capture_bounded_explicit_process_output_raw(invocation, policy);
        }();
        // Preserve primary process failure before post-process FS checks or
        // abort cleanup; child exit zero cannot erase parent cancellation.
        const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
        if(result.cancellation_signal || !exited || exited->exit_code != 0) {
            RecipeAcquisitionFailure failure{stage, result.cancellation_signal ? Reason::Cancelled : Reason::GitProcessFailed};
            failure.process = std::move(result);
            throw Failure(std::move(failure));
        }
        inspect(stage, GitInitializationState::AfterGitInit);
        return result;
    }

    void initialize() {
        const bool sha256 = expected.git_object_format() == GitObjectFormat::Sha256;
        run(Stage::Initialization, {"init", "--quiet", "--template=", "--initial-branch=recipe-acquisition", sha256 ? "--object-format=sha256" : "--object-format=sha1"}, 4096, GitInitializationState::BeforeGitInit, true);
        auto git_directory = open_beneath(repository.get(), ".git", O_RDONLY | O_DIRECTORY, Stage::Initialization);
        git_identity = status(git_directory.get(), Stage::Initialization);
        inspect(Stage::Initialization, GitInitializationState::AfterGitInit);
        // Add only the canonical identity required by the existing strict
        // full-review configuration contract. No origin observation is used.
        auto config = open_beneath(repository.get(), ".git/config", O_WRONLY | O_APPEND, Stage::Initialization);
        const auto before = status(config.get(), Stage::Initialization);
        const std::string origin = "\n[remote \"origin\"]\n\turl = " + expected.canonical_git_remote() +
                                   "\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n";
        std::size_t offset = 0;
        while(offset < origin.size()) {
            const auto written = ::write(config.get(), origin.data() + offset, origin.size() - offset);
            if(written < 0 && errno == EINTR) continue;
            if(written <= 0) fail(Stage::Initialization, Reason::IoFailure, errno);
            offset += static_cast<std::size_t>(written);
        }
        require_named(repository.get(), ".git/config", before, Stage::Initialization);
        config_bytes = read_regular(repository.get(), ".git/config", MAX_CONFIG_BYTES, Stage::Initialization);
        const auto output = run(Stage::RepositoryValidation, {"config", "--local", "--no-includes", "--null", "--list"}, MAX_CONFIG_BYTES).output;
        const auto format = trusted_git_recipe_acquisition_configuration_format(output, expected);
        if(!format) fail(Stage::RepositoryValidation, Reason::MalformedRepository);
        if(*format != expected.git_object_format()) fail(Stage::RepositoryValidation, Reason::ObjectFormatMismatch);
    }

    void verify(const std::string& metadata) {
        const auto& oid = *expected.target_revision().git_commit();
        const auto format = run(Stage::ExactCommit, {"rev-parse", "--show-object-format=storage"}, 16).output;
        if(format != (expected.git_object_format() == GitObjectFormat::Sha1 ? "sha1\n" : "sha256\n"))
            fail(Stage::ExactCommit, Reason::ObjectFormatMismatch);
        const auto objects = run(Stage::ExactCommit, {"cat-file", "--batch-all-objects", "--unordered", "--batch-check=%(objectname) %(objecttype) %(objectsize)"}, MAX_OBJECT_STREAM).output;
        std::string_view remaining(objects);
        std::size_t count = 0;
        std::uintmax_t bytes = 0;
        bool found = false;
        while(!remaining.empty()) {
            if(Clock::now() >= deadline) fail(Stage::ExactCommit, Reason::ResourceLimit);
            if(++count > MAX_ENTRIES) fail(Stage::ExactCommit, Reason::ResourceLimit);
            const auto newline = remaining.find('\n');
            const auto line = remaining.substr(0, newline);
            const auto first = line.find(' ');
            const auto second = first == std::string_view::npos ? first : line.find(' ', first + 1);
            if(newline == std::string_view::npos || first != oid.size() || second == std::string_view::npos)
                fail(Stage::ExactCommit, Reason::MalformedRepository);
            const auto object = line.substr(0, first);
            if(object.find_first_not_of("0123456789abcdef") != std::string_view::npos) fail(Stage::ExactCommit, Reason::MalformedRepository);
            const auto type = line.substr(first + 1, second - first - 1);
            if(type != "commit" && type != "tree" && type != "blob" && type != "tag") fail(Stage::ExactCommit, Reason::MalformedRepository);
            const auto size = parse_size(line.substr(second + 1), Stage::ExactCommit);
            if(size > MAX_OBJECT_BYTES || size > MAX_BYTES - bytes) fail(Stage::ExactCommit, Reason::ResourceLimit);
            bytes += size;
            if(object == oid) {
                if(found) fail(Stage::ExactCommit, Reason::MalformedRepository);
                found = true;
                if(type != "commit") fail(Stage::ExactCommit, Reason::WrongObjectType);
            }
            remaining.remove_prefix(newline + 1);
        }
        if(!found) fail(Stage::ExactCommit, Reason::ExpectedCommitUnavailable);
        if(run(Stage::ExactCommit, {"cat-file", "-t", oid}, 16).output != "commit\n") fail(Stage::ExactCommit, Reason::WrongObjectType);
        if(run(Stage::ExactCommit, {"rev-parse", "--verify", "--output-object-format=storage", "--end-of-options", oid + "^{commit}"}, 65).output != oid + "\n")
            fail(Stage::ExactCommit, Reason::ExactRevisionMismatch);
        // Check the exact tree entry (not a generated .SRCINFO, index, HEAD or
        // worktree file), then compare the cgit exact-id bytes without normalization.
        const auto entry = run(Stage::Metadata, {"ls-tree", "-z", oid, "--", ".SRCINFO"}, 128).output;
        const std::string prefix = entry.starts_with("100755 ") ? "100755 blob " : "100644 blob ";
        const std::string suffix("\t.SRCINFO\0", 10);
        if(!entry.starts_with(prefix) || !entry.ends_with(suffix) || entry.size() != prefix.size() + oid.size() + suffix.size())
            fail(Stage::Metadata, Reason::MetadataMismatch);
        const auto blob = entry.substr(prefix.size(), oid.size());
        if(blob.find_first_not_of("0123456789abcdef") != std::string::npos) fail(Stage::Metadata, Reason::MalformedRepository);
        const auto size_output = run(Stage::Metadata, {"cat-file", "-s", blob}, 32).output;
        if(size_output.empty() || size_output.back() != '\n') fail(Stage::Metadata, Reason::MalformedRepository);
        const auto size = parse_size(std::string_view(size_output).substr(0, size_output.size() - 1), Stage::Metadata);
        if(size > MAX_METADATA_BYTES) fail(Stage::Metadata, Reason::ResourceLimit);
        if(size != metadata.size() || run(Stage::Metadata, {"cat-file", "blob", blob}, MAX_METADATA_BYTES).output != metadata)
            fail(Stage::Metadata, Reason::MetadataMismatch);
        inspect(Stage::Metadata, GitInitializationState::AfterGitInit);
    }

    RecipeAcquisitionCleanupResult cleanup() noexcept {
        if(cleanup_attempted || !has_created_root) return cleanup_result;
        cleanup_attempted = true;
        try {
            notify(Stage::Cleanup, root_path);
            if(root.get() < 0 || root_identity.st_ino == 0) fail(Stage::Cleanup, Reason::IdentityChanged);
            require_lineage(Stage::Cleanup);
            Budget budget(Clock::now() + CLEANUP_TIMEOUT);
            std::vector<Inventory> plan;
            inventory(root.get(), root.get(), {}, root_identity.st_dev, 0, budget, Stage::Cleanup, plan);
            std::map<fs::path, struct stat> directory_identities;
            for(const auto& node : plan)
                if(S_ISDIR(node.identity.st_mode)) directory_identities.emplace(node.path, node.identity);
            for(const auto& node : plan) {
#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
                if(g_hooks.before_remove) g_hooks.before_remove(root_path);
#endif
                budget.check(Stage::Cleanup);
                // Every ancestor must still be the preflighted directory.
                // Matching only the leaf would allow a moved original child
                // beneath a replacement .git directory to authorize removal.
                for(auto ancestor = node.path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
                    auto descriptor = open_beneath(root.get(), ancestor, O_RDONLY | O_DIRECTORY, Stage::Cleanup);
                    if(!same_identity(directory_identities.at(ancestor), status(descriptor.get(), Stage::Cleanup))) fail(Stage::Cleanup, Reason::IdentityChanged);
                }
                require_lineage(Stage::Cleanup);
                auto parent_fd = open_beneath(root.get(), node.path.parent_path().empty() ? fs::path(".") : node.path.parent_path(), O_RDONLY | O_DIRECTORY, Stage::Cleanup);
                require_named(parent_fd.get(), node.path.filename(), node.identity, Stage::Cleanup);
                if(::unlinkat(parent_fd.get(), node.path.filename().c_str(), S_ISDIR(node.identity.st_mode) ? AT_REMOVEDIR : 0) != 0)
                    fail(Stage::Cleanup, Reason::IoFailure, errno);
                // Drop bridges only after deleting their exact directories;
                // retained root/parent identity remains required throughout.
                if(checkout && node.path == fs::path("moguet") / expected.package_base().package_base()) checkout.reset();
                if(node.path == "moguet") cache.reset();
            }
            require_lineage(Stage::Cleanup);
            if(::unlinkat(parent.get(), leaf.c_str(), AT_REMOVEDIR) != 0) fail(Stage::Cleanup, Reason::IoFailure, errno);
            has_created_root = false;
        } catch(const Failure& error) {
            cleanup_result = RecipeAcquisitionCleanupFailure{error.detail.reason, error.detail.error_number};
        } catch(const TrustedCacheError&) {
            cleanup_result = RecipeAcquisitionCleanupFailure{Reason::IdentityChanged, std::nullopt};
        } catch(...) {
            // No unreported retry by the destructor after explicit failure.
            // SIGKILL/power loss may leave residue; future invocations never adopt it.
            cleanup_result = RecipeAcquisitionCleanupFailure{Reason::IoFailure, std::nullopt};
        }
        return cleanup_result;
    }
};

InvocationOwnedRecipeAcquisition::InvocationOwnedRecipeAcquisition(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {
}
InvocationOwnedRecipeAcquisition::InvocationOwnedRecipeAcquisition(InvocationOwnedRecipeAcquisition&&) noexcept = default;
InvocationOwnedRecipeAcquisition::~InvocationOwnedRecipeAcquisition() noexcept = default;

const ValidatedCachePath& InvocationOwnedRecipeAcquisition::checkout() const {
    if(!state_ || state_->cleanup_attempted || !state_->checkout) throw std::logic_error("Recipe acquisition workspace is closed");
    state_->require_lineage(Stage::RepositoryValidation);
    return *state_->checkout;
}
const AurReviewedSourceReviewIdentity& InvocationOwnedRecipeAcquisition::identity() const {
    static_cast<void>(checkout());
    return state_->expected;
}
const fs::path& InvocationOwnedRecipeAcquisition::workspace_path() const {
    if(!state_) throw std::logic_error("Recipe acquisition owner was moved");
    return state_->root_path;
}
RecipeAcquisitionCleanupResult InvocationOwnedRecipeAcquisition::cleanup() noexcept {
    return state_ ? state_->cleanup() : RecipeAcquisitionCleanupResult{};
}

RecipeAcquisitionResult acquire_invocation_owned_recipe(const DevelTrackingBootstrapTrial& trial) {
    std::unique_ptr<InvocationOwnedRecipeAcquisition::State> state;
    RecipeAcquisitionFailure failure{Stage::WorkspaceCreation, Reason::IoFailure};
    try {
        state = std::make_unique<InvocationOwnedRecipeAcquisition::State>(trial);
        if(trial.source_metadata().empty() || trial.source_metadata().size() > MAX_METADATA_BYTES)
            fail(Stage::Metadata, Reason::ResourceLimit);
        state->create();
        state->initialize();
        state->run(Stage::Fetch, {"fetch", "--no-tags", "--no-recurse-submodules", "--no-auto-maintenance", "--no-write-commit-graph", "--no-write-fetch-head", "--", state->expected.canonical_git_remote(), *state->expected.target_revision().git_commit()}, 65536, GitInitializationState::AfterGitInit, true);
        state->verify(trial.source_metadata());
        return InvocationOwnedRecipeAcquisition(std::move(state));
    } catch(Failure& error) {
        failure = std::move(error.detail);
    } catch(const TrustedCacheError& error) {
        failure.boundary_failure = error.failure();
        failure.stage = state ? state->active_stage : Stage::WorkspaceCreation;
        failure.reason = Reason::UnsafeFilesystem;
        if(error.failure().system_error) failure.error_number = error.failure().system_error->value();
    } catch(const xdg_directory_safety::PreparationError& error) {
        failure.stage = state ? state->active_stage : Stage::WorkspaceCreation;
        failure.reason = Reason::UnsafeFilesystem;
        if(error.failure().system_error) failure.error_number = error.failure().system_error->value();
    } catch(const std::invalid_argument&) {
        failure.stage = state ? state->active_stage : Stage::WorkspaceCreation;
        failure.reason = Reason::MalformedRepository;
    } catch(const std::bad_alloc&) {
        failure.stage = state ? state->active_stage : Stage::WorkspaceCreation;
        failure.reason = Reason::ResourceLimit;
    } catch(const std::system_error& error) {
        failure.stage = state ? state->active_stage : Stage::WorkspaceCreation;
        failure.error_number = error.code().value();
        failure.reason = Reason::IoFailure;
    } catch(const std::exception&) {
        failure.stage = state ? state->active_stage : Stage::WorkspaceCreation;
        failure.reason = Reason::IoFailure;
    }
    if(state) {
        failure.cleanup = state->cleanup();
        if(failure.cleanup) failure.abandoned_root = state->root_path;
    }
    return failure;
}

#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
void set_recipe_acquisition_test_hooks(RecipeAcquisitionTestHooks hooks) {
    g_hooks = std::move(hooks);
}
#endif
