#include "pinned_submodule_closure.hpp"
#include "git_remote_revision_observer.hpp"
#include "trusted_git_process_policy.hpp"
#include "logging.hpp"
#include "localization.hpp"
#include "shell_words.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <map>
#include <set>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Stage = PinnedClosureStage;
using Reason = PinnedClosureFailureReason;

struct RootTagCommandPresentation {
    PresentationDetail detail;
    std::size_t additional_objects;
};

// Post-fetch storage bounds are not hard network or disk quotas.
constexpr std::size_t MAX_ENTRIES = 262144;
constexpr std::size_t MAX_DEPTH = 64;
constexpr std::uintmax_t MAX_BYTES = 1024ULL * 1024 * 1024;
constexpr std::size_t MAX_BLOB_BYTES = 64 * 1024 * 1024;
constexpr std::size_t MAX_CONFIG_BYTES = 8192;
constexpr std::size_t MAX_TAG_OBSERVATION_BYTES = 1024 * 1024;
constexpr std::size_t MAX_TAG_OBJECT_BYTES = 256 * 1024;
constexpr auto ACQUISITION_TIMEOUT = std::chrono::minutes(10);
constexpr auto CLEANUP_TIMEOUT = std::chrono::seconds(5);
constexpr std::size_t MAX_PATH_BYTES = 4096;
constexpr std::size_t MAX_COMPONENT_BYTES = 255;

#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
PinnedClosureTestHooks g_hooks;
#endif

void notify(Stage stage, const fs::path& path) {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
    if(g_hooks.event) g_hooks.event(stage, path);
#else
    static_cast<void>(stage);
    static_cast<void>(path);
#endif
}

class Failure final : public std::exception {
public:
    PinnedClosureFailure detail;
    explicit Failure(PinnedClosureFailure value) : detail(std::move(value)) {
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
    }
    void check(Stage stage) const {
        if(Clock::now() >= deadline) fail(stage, Reason::ResourceLimitExceeded);
    }
};

// Root-relative NO_XDEV/NO_SYMLINKS opens and post-open identity comparison
// cover the Git DB as well as its config/refs. No worktree-content authority is
// minted here; reviewed byte/PKGBUILD rules remain with full review and S3.
void inventory(int root, int directory, const fs::path& prefix, dev_t device,
               std::size_t depth, Budget& budget, Stage stage, std::vector<Inventory>& result) {
    budget.check(stage);
    if(depth > MAX_DEPTH) fail(stage, Reason::ResourceLimitExceeded);
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
        if(++budget.entries > budget.entry_limit) fail(stage, Reason::ResourceLimitExceeded);
        struct stat named{};
        if(::fstatat(directory, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) fail(stage, Reason::IoFailure, errno);
        if((!S_ISREG(named.st_mode) && !S_ISDIR(named.st_mode)) || named.st_uid != ::geteuid() ||
           named.st_dev != device || (named.st_mode & 0022) != 0 ||
           (S_ISREG(named.st_mode) && named.st_nlink != 1)) fail(stage, Reason::UnsafeFilesystem);
        if(named.st_size < 0) fail(stage, Reason::UnsafeFilesystem);
        const auto bytes = S_ISREG(named.st_mode) ? static_cast<std::uintmax_t>(named.st_size) : 0;
        if(bytes > budget.byte_limit - budget.bytes) fail(stage, Reason::ResourceLimitExceeded);
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
    if(static_cast<std::uintmax_t>(before.st_size) > limit) fail(stage, Reason::ResourceLimitExceeded);
    std::string bytes;
    std::array<char, 4096> buffer{};
    while(true) {
        const auto count = ::read(fd.get(), buffer.data(), buffer.size());
        if(count < 0 && errno == EINTR) continue;
        if(count < 0) fail(stage, Reason::IoFailure, errno);
        if(count == 0) break;
        if(static_cast<std::size_t>(count) > limit - bytes.size()) fail(stage, Reason::ResourceLimitExceeded);
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    require_named(root, path, before, stage);
    const auto after = status(fd.get(), stage);
    if(!same_identity(before, after) || before.st_size != after.st_size || bytes.size() != static_cast<std::size_t>(after.st_size))
        fail(stage, Reason::IdentityChanged);
    return bytes;
}

} // namespace

namespace {
void safe_path(std::string_view path) {
    if(path.empty() || path.size() > MAX_PATH_BYTES) fail(Stage::Declaration, Reason::UnsafePath);
    while(true) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if(part.empty() || part == "." || part == ".." || part == ".git" || part.size() > MAX_COMPONENT_BYTES)
            fail(Stage::Declaration, Reason::UnsafePath);
        for(unsigned char byte : part)
            if(byte < 32 || byte == 127 || byte == '\\') fail(Stage::Declaration, Reason::UnsafePath);
        if(slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
}
std::string_view trim(std::string_view value) {
    while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while(!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}
// Deliberately closed Git-config subset: quoted strings and ordinary trailing
// comments, but no continuation/include/implicit boolean/legacy section syntax.
std::string config_value(std::string_view value) {
    value = trim(value);
    bool quoted = false;
    std::string result;
    std::size_t significant = 0;
    for(std::size_t i = 0; i < value.size(); ++i) {
        const char byte = value[i];
        if(byte == '"') {
            quoted = !quoted;
            continue;
        }
        if(!quoted && (byte == '#' || byte == ';')) break;
        if(byte == '\\') {
            if(++i == value.size()) fail(Stage::Declaration, Reason::MalformedDeclaration);
            const char escaped = value[i];
            if(escaped != '\\' && escaped != '"') fail(Stage::Declaration, Reason::MalformedDeclaration);
            result += escaped;
            significant = result.size();
        } else {
            if(static_cast<unsigned char>(byte) < 32 && byte != '\t') fail(Stage::Declaration, Reason::MalformedDeclaration);
            result += byte;
            if(quoted || (byte != ' ' && byte != '\t')) significant = result.size();
        }
    }
    if(quoted) fail(Stage::Declaration, Reason::MalformedDeclaration);
    result.resize(significant);
    return result;
}
struct Declaration {
    std::string name, path, locator;
};
std::vector<Declaration> declarations(std::string_view bytes) {
    std::vector<Declaration> result;
    std::set<std::string> names, paths, keys;
    auto finish = [&] {
        if(result.empty()) return;
        const auto& entry = result.back();
        if(!keys.contains("path") || !keys.contains("url")) fail(Stage::Declaration, Reason::DeclarationMismatch);
        safe_path(entry.path);
        if(!paths.insert(entry.path).second) fail(Stage::Declaration, Reason::DeclarationMismatch);
    };
    while(!bytes.empty()) {
        const auto newline = bytes.find('\n');
        auto line = trim(bytes.substr(0, newline));
        bytes.remove_prefix(newline == std::string_view::npos ? bytes.size() : newline + 1);
        if(line.empty() || line.front() == '#' || line.front() == ';') continue;
        if(line.front() == '[') {
            finish();
            constexpr std::string_view PREFIX = "[submodule \"";
            if(!line.starts_with(PREFIX) || !line.ends_with("\"]")) fail(Stage::Declaration, Reason::MalformedDeclaration);
            const auto raw_name = line.substr(PREFIX.size(), line.size() - PREFIX.size() - 2);
            // Escapes in logical names would introduce a second namespace
            // spelling. This subset requires an exact literal safe name.
            if(raw_name.find('"') != std::string_view::npos) fail(Stage::Declaration, Reason::MalformedDeclaration);
            safe_path(raw_name);
            std::string name(raw_name);
            if(!names.insert(name).second) fail(Stage::Declaration, Reason::DeclarationMismatch);
            result.push_back({std::move(name), {}, {}});
            keys.clear();
            continue;
        }
        const auto equal = line.find('=');
        if(result.empty() || equal == std::string_view::npos) fail(Stage::Declaration, Reason::MalformedDeclaration);
        std::string key(trim(line.substr(0, equal)));
        for(char& c : key)
            if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if(!keys.insert(key).second) fail(Stage::Declaration, Reason::MalformedDeclaration);
        const auto value = config_value(line.substr(equal + 1));
        if(key == "path")
            result.back().path = value;
        else if(key == "url") {
            try {
                result.back().locator = ValidatedHttpsGitRemote::make(value).canonical_url();
            } catch(const std::invalid_argument&) {
                fail(Stage::Declaration, Reason::UnsupportedTransport);
            }
        } else if(key != "update" || value != "checkout")
            fail(Stage::Declaration, Reason::UnsupportedUpdatePolicy);
    }
    finish();
    for(const auto& path : paths) {
        for(auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
            if(paths.contains(path.substr(0, slash))) fail(Stage::Declaration, Reason::UnsafePath);
    }
    return result;
}
bool hex(std::string_view value) {
    return !value.empty() && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
struct Repository {
    std::string leaf;
    Descriptor descriptor;
    struct stat identity{};
    std::string config;
    bool initialized = false;
};
} // namespace

// This backing is intentionally not a friend of the success owner. Defining a
// same-named backing in another TU cannot open a private success constructor.
struct PinnedSubmoduleClosureData {
    EvaluatedDevelSourceSelection selection;
    PinnedClosureLimits limits;
    fs::path root_path;
    std::string leaf;
    Descriptor parent, root;
    struct stat parent_identity{}, root_identity{};
    bool created = false, closed = false, objects_cleanup_attempted = false;
    PinnedClosureCleanupResult cleanup_result;
    std::vector<Repository> repositories;
    std::vector<PinnedSubmoduleNode> nodes;
    std::vector<PinnedSubmoduleEdge> edges;
    std::vector<PinnedRootTag> root_tags;
    Clock::time_point deadline = Clock::now() + ACQUISITION_TIMEOUT;
    std::size_t process_count = 0, tree_count = 0, metadata_bytes = 0, declaration_bytes = 0;
    Stage active = Stage::Input;
    std::vector<std::string> environment;

    explicit PinnedSubmoduleClosureData(EvaluatedDevelSourceSelection value) : selection(std::move(value)) {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
        if(g_hooks.limits) limits = *g_hooks.limits;
#endif
    }
    ~PinnedSubmoduleClosureData() noexcept {
        static_cast<void>(cleanup());
    }
    void check() const {
        if(Clock::now() >= deadline) fail(active, Reason::ResourceLimitExceeded);
    }
    void account(std::size_t amount) {
        check();
        if(amount > limits.metadata_bytes - metadata_bytes) fail(active, Reason::ResourceLimitExceeded);
        metadata_bytes += amount;
    }
    void lineage() const {
        if(!created || root.get() < 0) fail(active, Reason::IdentityChanged);
        Descriptor current(::open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if(current.get() < 0 || !same_identity(parent_identity, status(current.get(), active))) fail(active, Reason::IdentityChanged);
        require_named(parent.get(), leaf, root_identity, active);
        if(!same_identity(root_identity, status(root.get(), active))) fail(active, Reason::IdentityChanged);
        require_private(root_identity, parent_identity.st_dev, active);
        for(const auto& repo : repositories) {
            require_named(root.get(), repo.leaf, repo.identity, active);
            if(!same_identity(repo.identity, status(repo.descriptor.get(), active))) fail(active, Reason::IdentityChanged);
        }
    }
    void create() {
        active = Stage::WorkspaceCreation;
        environment = trusted_git_process_environment(TrustedGitProcessEnvironmentMode::ManagedOperation);
        parent = Descriptor(::open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if(parent.get() < 0) fail(active, Reason::IoFailure, errno);
        parent_identity = status(parent.get(), active);
        if((parent_identity.st_uid != 0 && parent_identity.st_uid != ::geteuid()) ||
           ((parent_identity.st_mode & 0022) && !(parent_identity.st_mode & S_ISVTX))) fail(active, Reason::UnsafeFilesystem);
        for(unsigned attempt = 0; attempt < 32; ++attempt) {
            leaf = "moguet-pinned-closure-" + random_suffix();
            if(::mkdirat(parent.get(), leaf.c_str(), 0700) == 0) {
                created = true;
                break;
            }
            if(errno != EEXIST) fail(active, Reason::CreationFailed, errno);
        }
        if(!created) fail(active, Reason::CreationFailed);
        root_path = fs::path("/tmp") / leaf;
        root = open_beneath(parent.get(), leaf, O_RDONLY | O_DIRECTORY, active);
        root_identity = status(root.get(), active);
        lineage();
    }
    void inspect(std::optional<Clock::time_point> phase_deadline = std::nullopt) {
        lineage();
        Budget budget(phase_deadline.value_or(deadline));
        std::vector<Inventory> entries;
        inventory(root.get(), root.get(), {}, root_identity.st_dev, 0, budget, active, entries);
        for(const auto& entry : entries) {
            const auto first = entry.path.begin()->string();
            if(std::none_of(repositories.begin(), repositories.end(), [&](const auto& repo) { return first == repo.leaf; }))
                fail(active, Reason::MalformedRepository);
        }
        for(const auto& repo : repositories) {
            for(const auto& entry : entries) {
                auto relative = entry.path.lexically_relative(repo.leaf).generic_string();
                if(relative == "." || relative.starts_with("../")) continue;
                if(!repo.initialized) fail(active, Reason::MalformedRepository);
                bool allowed = false;
                if(S_ISDIR(entry.identity.st_mode)) {
                    allowed = relative == "objects" || relative == "objects/info" || relative == "objects/pack" ||
                              relative == "refs" || relative == "refs/heads" || relative == "refs/tags" ||
                              (relative.size() == 10 && relative.starts_with("objects/") && hex(std::string_view(relative).substr(8)));
                } else {
                    allowed = relative == "config" || relative == "HEAD";
                    if(relative.starts_with("objects/") && relative.size() > 11 && relative[10] == '/') {
                        const auto hash_path = std::string_view(relative).substr(8);
                        allowed = hex(hash_path.substr(0, 2)) && hex(hash_path.substr(3)) &&
                                  (hash_path.size() == 41 || hash_path.size() == 65);
                    }
                    if(relative.starts_with("objects/pack/pack-")) {
                        const auto object = std::string_view(relative).substr(18);
                        const auto dot = object.find('.');
                        allowed = (dot == 40 || dot == 64) && hex(object.substr(0, dot)) &&
                                  (object.substr(dot) == ".pack" || object.substr(dot) == ".idx" || object.substr(dot) == ".rev");
                    }
                }
                if(!allowed) fail(active, Reason::MalformedRepository);
            }
            if(repo.initialized && !repo.config.empty() && read_regular(repo.descriptor.get(), "config", MAX_CONFIG_BYTES, active) != repo.config)
                fail(active, Reason::MalformedRepository);
        }
        lineage();
    }
    std::string run(Stage stage, std::optional<std::size_t> repository, std::vector<std::string> operation,
                    std::size_t limit, bool initializing = false,
                    std::optional<RootTagCommandPresentation> root_tag_presentation = std::nullopt) {
        active = stage;
        notify(stage, root_path);
        check();
        inspect();
        if(++process_count > limits.processes) fail(active, Reason::ResourceLimitExceeded);
        auto arguments = repository ? trusted_git_recipe_acquisition_process_arguments() : trusted_git_observer_process_arguments();
        if(repository) arguments.push_back("--git-dir=.");
        arguments.insert(arguments.end(), operation.begin(), operation.end());
        Descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if(input.get() < 0) fail(active, Reason::IoFailure, errno);
        ExplicitProcessInvocation invocation{"/usr/bin/git", std::move(arguments), environment};
        invocation.working_directory_fd = repository ? repositories.at(*repository).descriptor.get() : root.get();
        invocation.standard_input_fd = input.get();
        if(root_tag_presentation) {
            // Project the actual executable/argv, including trusted Git options.
            // This serialization is diagnostic only; execution stays structured.
            const auto command = shell_words::quote(invocation.executable) + " " + shell_words::join(invocation.arguments);
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
            if(g_hooks.root_tag_command) g_hooks.root_tag_command(root_tag_presentation->detail, command);
#endif
            switch(root_tag_presentation->detail) {
                case PresentationDetail::Normal:
                    // TRANSLATORS: The placeholder counts distinct additional raw Git objects, not tag names.
                    Logger::command(command, localization::format_translated_message(
                                                 "Fetching root upstream tag objects ({} objects)",
                                                 root_tag_presentation->additional_objects));
                    break;
                case PresentationDetail::Detailed:
                    Logger::raw_cmd(command);
                    break;
            }
        }
        // Terminal and state-log I/O must consume the acquisition budget too.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if(remaining.count() <= 0) fail(active, Reason::ResourceLimitExceeded);
        const BoundedProcessPolicy policy{remaining, std::chrono::milliseconds(200), limit, true, false};
        auto result = [&] {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
            if(g_hooks.process) return g_hooks.process(invocation, policy);
#endif
            return capture_bounded_explicit_process_output_raw(invocation, policy);
        }();
        const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
        if(result.cancellation_signal || !exited || exited->exit_code != 0) {
            // Only an ordinary failed exact fetch reports object unavailability.
            // Init and process infrastructure failures retain their own reason.
            const bool unavailable = exited && exited->exit_code != 0 &&
                                     (stage == Stage::ChildAcquisition || stage == Stage::RootAcquisition) &&
                                     !operation.empty() && operation.front() == "fetch";
            PinnedClosureFailure failure{stage, result.cancellation_signal ? Reason::Cancelled : unavailable ? Reason::PinnedObjectUnavailable
                                                                                                             : Reason::GitProcessFailed};
            failure.process = std::move(result);
            throw Failure(std::move(failure));
        }
        if(result.output.size() > limit) fail(stage, Reason::ResourceLimitExceeded);
        if(initializing) repositories.at(*repository).initialized = true;
        inspect();
        account(result.output.size());
        return std::move(result.output);
    }
    ReviewedSourceObjectId observe() {
        const auto& source = selection.git_source();
        const auto remote = ValidatedHttpsGitRemote::make(source.source_location()).canonical_url();
        const bool is_default = source.selector().kind() == VcsSelectorKind::DefaultHead;
        if(!is_default && source.selector().kind() != VcsSelectorKind::Branch) fail(Stage::Input, Reason::InvalidSelection);
        const std::string ref = is_default ? "HEAD" : "refs/heads/" + *source.selector().value();
        Logger::raw_cmd("git ls-remote --exit-code " + remote + " " + ref + " refs/tags/*");
        const auto output = run(Stage::RootObservation, std::nullopt,
                                {"ls-remote", "--exit-code", "--", remote, ref, "refs/tags/*"}, MAX_TAG_OBSERVATION_BYTES);
        // Patterns also match tails. Validate every complete record rather than
        // treating ls-remote's pattern selection as exact namespace authority.
        if(output.empty() || output.back() != '\n') fail(active, Reason::MalformedObservation);
        std::optional<ReviewedSourceObjectId> root_oid;
        std::map<std::string, ReviewedSourceObjectId> tags, peeled;
        try {
            for(std::size_t begin = 0; begin < output.size();) {
                const auto end = output.find('\n', begin);
                const auto tab = output.find('\t', begin);
                if(tab == std::string::npos || tab >= end) fail(active, Reason::MalformedObservation);
                auto oid = ReviewedSourceObjectId::make(output.substr(begin, tab - begin));
                auto name = output.substr(tab + 1, end - tab - 1);
                if(name == ref) {
                    if(root_oid) fail(active, Reason::MalformedObservation);
                    root_oid = oid;
                } else {
                    const bool is_peeled = name.ends_with("^{}");
                    if(is_peeled) name.resize(name.size() - 3);
                    if(!name.starts_with("refs/tags/") || name.size() <= 10 || name.size() > MAX_PATH_BYTES ||
                       name.find_first_of("\0\r\t\n", 0, 4) != std::string::npos)
                        fail(active, Reason::MalformedObservation);
                    auto& mapping = is_peeled ? peeled : tags;
                    if(!mapping.emplace(std::move(name), oid).second) fail(active, Reason::MalformedObservation);
                    if(mapping.size() > limits.root_tags) fail(active, Reason::ResourceLimitExceeded);
                }
                begin = end + 1;
            }
            if(!root_oid) fail(active, Reason::MalformedObservation);
            for(const auto& [name, oid] : peeled)
                if(!tags.contains(name)) fail(active, Reason::MalformedObservation);
            for(const auto& [name, oid] : tags) {
                const auto found = peeled.find(name);
                std::optional<ReviewedSourceObjectId> terminal;
                if(found != peeled.end()) terminal = found->second;
                if(oid.format() != root_oid->format() || (terminal && terminal->format() != root_oid->format()))
                    fail(active, Reason::MalformedObservation);
                // Delegate the complete ref-name grammar to Git. A nonzero
                // validation result is malformed observation, not a fallback.
                try {
                    run(Stage::RootObservation, std::nullopt, {"check-ref-format", name}, 4096);
                } catch(Failure& error) {
                    if(error.detail.process) {
                        const auto* exited = std::get_if<BoundedProcessExited>(&error.detail.process->outcome);
                        if(!error.detail.process->cancellation_signal && exited && exited->exit_code != 0)
                            error.detail.reason = Reason::MalformedObservation;
                    }
                    throw;
                }
                root_tags.push_back(PinnedRootTag(name, oid, terminal));
            }
            return *root_oid;
        } catch(const std::invalid_argument&) {
            fail(active, Reason::MalformedObservation);
        }
    }
    void acquire_root_tags(std::size_t index, const std::string& locator, const ReviewedSourceObjectId& root_oid, PresentationDetail presentation_detail) {
        // Fetch only observed raw OIDs, never names. Object-only backing stays
        // ref-free, including for annotated and non-reachable tags.
        std::set<std::string> fetched{root_oid.value()};
        std::vector<std::string> fetch{"fetch", "--no-tags", "--no-recurse-submodules", "--no-auto-maintenance", "--no-write-commit-graph", "--no-write-fetch-head", "--", locator};
        for(const auto& tag : root_tags) {
            if(fetched.insert(tag.raw().value()).second) {
                fetch.push_back(tag.raw().value());
            }
        }
        // One bounded argv (at most root_tags full OIDs) avoids a separate
        // HTTPS negotiation per tag without ever resolving a name again.
        if(fetched.size() > 1) {
            // The same insertion decision appends argv and counts objects;
            // the initial root X is already fetched and is not additional.
            run(Stage::RootAcquisition, index, std::move(fetch), 65536, false,
                RootTagCommandPresentation{presentation_detail, fetched.size() - 1});
        }
        std::vector<std::string> proof{"fsck", "--strict", "--no-reflogs", "--no-dangling", root_oid.value()};
        for(const auto& tag : root_tags)
            proof.push_back(tag.raw().value());
        run(Stage::ObjectProof, index, std::move(proof), 65536);
        for(const auto& tag : root_tags) {
            auto current = tag.raw();
            std::size_t depth = 0;
            while(true) {
                const auto type = run(Stage::ObjectProof, index, {"cat-file", "-t", current.value()}, 16);
                if(type != "tag\n") {
                    if(type != "commit\n" && type != "tree\n" && type != "blob\n") fail(active, Reason::UnexpectedObjectType);
                    if((depth != 0) != tag.peeled().has_value() || (tag.peeled() && current != *tag.peeled()))
                        fail(active, Reason::MalformedObservation);
                    break;
                }
                if(++depth > limits.tag_depth) fail(active, Reason::ResourceLimitExceeded);
                const auto bytes = run(Stage::ObjectProof, index, {"cat-file", "tag", current.value()}, MAX_TAG_OBJECT_BYTES);
                const auto end = bytes.find('\n');
                if(!bytes.starts_with("object ") || end != 7 + root_oid.value().size()) fail(active, Reason::UnexpectedObjectType);
                try {
                    current = ReviewedSourceObjectId::make(bytes.substr(7, end - 7));
                } catch(const std::invalid_argument&) {
                    fail(active, Reason::UnexpectedObjectType);
                }
                if(current.format() != root_oid.format()) fail(active, Reason::ObjectFormatMismatch);
            }
        }
    }
    std::size_t acquire(const std::string& locator, const ReviewedSourceObjectId& oid, bool is_root, PresentationDetail presentation_detail) {
        active = is_root ? Stage::RootAcquisition : Stage::ChildAcquisition;
        check();
        lineage();
        const auto index = repositories.size();
        const auto name = "node-" + std::to_string(index);
        if(::mkdirat(root.get(), name.c_str(), 0700) != 0) fail(active, Reason::CreationFailed, errno);
        auto descriptor = open_beneath(root.get(), name, O_RDONLY | O_DIRECTORY, active);
        const auto identity = status(descriptor.get(), active);
        require_private(identity, root_identity.st_dev, active);
        repositories.push_back({name, std::move(descriptor), identity, {}, false});
        const bool sha256 = oid.format() == GitObjectFormat::Sha256;
        run(active, index, {"init", "--bare", "--quiet", "--template=", "--initial-branch=pinned-closure", sha256 ? "--object-format=sha256" : "--object-format=sha1"}, 4096, true);
        auto& repo = repositories[index];
        const auto config = read_regular(repo.descriptor.get(), "config", MAX_CONFIG_BYTES, active);
        const std::string expected = std::string(sha256 ? "[extensions]\n\tobjectformat = sha256\n" : "") +
                                     "[core]\n\trepositoryformatversion = " + (sha256 ? "1" : "0") + "\n\tfilemode = true\n\tbare = true\n";
        if(config != expected) fail(active, Reason::MalformedRepository);
        repo.config = config;
        Logger::raw_cmd("git fetch --no-tags --no-recurse-submodules " + locator + " " + oid.value());
        run(active, index, {"fetch", "--no-tags", "--no-recurse-submodules", "--no-auto-maintenance", "--no-write-commit-graph", "--no-write-fetch-head", "--", locator, oid.value()}, 65536);
        if(run(Stage::ObjectProof, index, {"rev-parse", "--show-object-format=storage"}, 16) != (sha256 ? "sha256\n" : "sha1\n")) fail(active, Reason::ObjectFormatMismatch);
        if(run(Stage::ObjectProof, index, {"cat-file", "-t", oid.value()}, 16) != "commit\n") fail(active, Reason::UnexpectedObjectType);
        if(run(Stage::ObjectProof, index, {"rev-parse", "--verify", "--output-object-format=storage", "--end-of-options", oid.value() + "^{commit}"}, 65) != oid.value() + "\n") fail(active, Reason::ObjectFormatMismatch);
        // fsck validates raw hashes and connectivity including tree/blob backing;
        // parent gitlinks deliberately do not claim child object availability.
        run(Stage::ObjectProof, index, {"fsck", "--strict", "--no-reflogs", "--no-dangling", oid.value()}, 65536);
        if(is_root) acquire_root_tags(index, locator, oid, presentation_detail);
        return index;
    }
    std::string blob(std::size_t repository, const ReviewedSourceFileVersion& entry, std::size_t limit) {
        if(entry.mode() == ReviewedSourceFileMode::Gitlink || !entry.blob_size()) fail(active, Reason::UnexpectedObjectType);
        if(*entry.blob_size() > limit) fail(active, Reason::ResourceLimitExceeded);
        if(run(Stage::ObjectProof, repository, {"cat-file", "-t", entry.object_id().value()}, 16) != "blob\n") fail(active, Reason::UnexpectedObjectType);
        auto bytes = run(Stage::Declaration, repository, {"cat-file", "blob", entry.object_id().value()}, limit);
        if(bytes.size() != *entry.blob_size()) fail(active, Reason::MalformedTree);
        return bytes;
    }
    void traverse(const std::string& locator, const ReviewedSourceObjectId& oid, std::optional<std::size_t> parent_edge,
                  std::size_t depth, std::set<std::pair<std::string, std::string>>& ancestry, PresentationDetail presentation_detail) {
        active = Stage::RecursiveTraversal;
        check();
        if(depth > limits.depth) fail(active, Reason::ResourceLimitExceeded);
        const auto key = std::make_pair(locator, oid.value());
        if(!ancestry.insert(key).second) fail(active, Reason::RecursiveCycle);
        const auto index = acquire(locator, oid, !parent_edge, presentation_detail);
        auto raw = run(Stage::ObjectProof, index, {"cat-file", "commit", oid.value()}, 1024 * 1024);
        const auto newline = raw.find('\n');
        if(newline != 5 + oid.value().size() || !raw.starts_with("tree ")) fail(active, Reason::UnexpectedObjectType);
        const auto tree = ReviewedSourceObjectId::make(raw.substr(5, newline - 5));
        if(tree.format() != oid.format()) fail(active, Reason::ObjectFormatMismatch);
        if(run(Stage::ObjectProof, index, {"cat-file", "-t", tree.value()}, 16) != "tree\n") fail(active, Reason::UnexpectedObjectType);
        const auto metadata = run(Stage::RecursiveTraversal, index, {"ls-tree", "-r", "-z", "--full-tree", "--no-abbrev", "--format=%(objectmode)%x00%(objecttype)%x00%(objectname)%x00%(objectsize)", tree.value(), "--"}, 32 * 1024 * 1024);
        const auto paths = run(Stage::RecursiveTraversal, index, {"ls-tree", "-r", "-z", "--full-tree", "--name-only", tree.value(), "--"}, 32 * 1024 * 1024);
        const auto records = static_cast<std::size_t>(std::count(paths.begin(), paths.end(), '\0'));
        if(records > limits.tree_records - tree_count) fail(active, Reason::ResourceLimitExceeded);
        tree_count += records;
        auto parsed = parse_reviewed_source_tree_output(metadata, paths, oid.format(), ReviewedSourceMachineStream::TargetTree);
        auto* inventory_value = std::get_if<ReviewedSourceTreeInventory>(&parsed);
        if(!inventory_value) fail(active, Reason::MalformedTree);
        for(const auto& entry : inventory_value->entries)
            safe_path(entry.path().raw_bytes());
        nodes.push_back({locator, oid, tree, std::move(*inventory_value), parent_edge});
        const auto inventory_copy = nodes[index].inventory;
        std::vector<Declaration> declared;
        std::map<std::string, ReviewedSourceObjectId> links;
        for(const auto& entry : inventory_copy.entries) {
            if(entry.mode() == ReviewedSourceFileMode::Gitlink) links.emplace(entry.path().raw_bytes(), entry.object_id());
            if(entry.path().raw_bytes() == ".gitmodules") {
                if(entry.mode() != ReviewedSourceFileMode::Regular && entry.mode() != ReviewedSourceFileMode::Executable) fail(Stage::Declaration, Reason::DeclarationMismatch);
                if(!entry.blob_size() || *entry.blob_size() > limits.declaration_bytes || *entry.blob_size() > limits.aggregate_declaration_bytes - declaration_bytes)
                    fail(Stage::Declaration, Reason::ResourceLimitExceeded);
                declaration_bytes += *entry.blob_size();
                declared = declarations(blob(index, entry, limits.declaration_bytes));
            }
        }
        if(declared.size() != links.size()) fail(Stage::Declaration, Reason::DeclarationMismatch);
        for(const auto& declaration : declared)
            if(!links.contains(declaration.path)) fail(Stage::Declaration, Reason::DeclarationMismatch);
        // Correlate the entire parent before the first child acquisition.
        for(const auto& declaration : declared) {
            if(edges.size() >= limits.edges) fail(Stage::RecursiveTraversal, Reason::ResourceLimitExceeded);
            const auto pin = links.at(declaration.path);
            const auto edge = edges.size();
            const auto child = nodes.size();
            edges.push_back({index, child, declaration.name, declaration.path, declaration.locator, pin});
            traverse(declaration.locator, pin, edge, depth + 1, ancestry, presentation_detail);
        }
        ancestry.erase(key);
    }
    void cleanup_objects() noexcept {
        if(objects_cleanup_attempted) return;
        // Physical release is terminal even on refusal. Semantic selection and
        // Accepted lineage remain live until the enclosing owner is cleaned.
        objects_cleanup_attempted = true;
        if(created) try {
                active = Stage::Cleanup;
                notify(active, root_path);
                lineage();
                Budget budget(Clock::now() + CLEANUP_TIMEOUT);
                std::vector<Inventory> plan;
                inventory(root.get(), root.get(), {}, root_identity.st_dev, 0, budget, active, plan);
                std::map<fs::path, struct stat> directories;
                for(const auto& node : plan)
                    if(S_ISDIR(node.identity.st_mode)) directories.emplace(node.path, node.identity);
                // Repository lineage is checked before deletion. Thereafter each
                // planned ancestor is rechecked; deleted repos are no longer live.
                repositories.clear();
                for(const auto& node : plan) {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
                    if(g_hooks.before_remove) g_hooks.before_remove(root_path);
#endif
                    budget.check(active);
                    lineage();
                    for(auto ancestor = node.path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
                        auto fd = open_beneath(root.get(), ancestor, O_RDONLY | O_DIRECTORY, active);
                        if(!same_identity(directories.at(ancestor), status(fd.get(), active))) fail(active, Reason::IdentityChanged);
                    }
                    auto fd = open_beneath(root.get(), node.path.parent_path().empty() ? fs::path(".") : node.path.parent_path(), O_RDONLY | O_DIRECTORY, active);
                    require_named(fd.get(), node.path.filename(), node.identity, active);
                    if(::unlinkat(fd.get(), node.path.filename().c_str(), S_ISDIR(node.identity.st_mode) ? AT_REMOVEDIR : 0) != 0) fail(active, Reason::IoFailure, errno);
                }
                lineage();
                if(::unlinkat(parent.get(), leaf.c_str(), AT_REMOVEDIR) != 0) fail(active, Reason::IoFailure, errno);
                created = false;
            } catch(const Failure& failure) {
                cleanup_result.objects = PinnedClosureCleanupFailure{failure.detail.reason, failure.detail.error_number};
            } catch(...) {
                cleanup_result.objects = PinnedClosureCleanupFailure{Reason::IoFailure, std::nullopt};
            }
        repositories.clear();
        root = Descriptor();
        parent = Descriptor();
    }
    PinnedClosureCleanupResult cleanup() noexcept {
        if(closed) return cleanup_result;
        closed = true;
        cleanup_objects();
        const auto selected = selection.cleanup();
        if(const auto* failure = std::get_if<InvocationOwnedSourceBuildContextFailure>(&selected)) cleanup_result.selection = *failure;
        return cleanup_result;
    }
};

std::optional<PinnedClosureFailure> PinnedSubmoduleWorkspaceAuthority::release_acquisition_backing(
    InvocationOwnedPinnedSubmoduleClosure& closure) {
    if(!closure.valid()) return PinnedClosureFailure{Stage::Input, Reason::InvalidSelection};
    auto& data = *closure.data_;
    data.cleanup_objects();
    if(!data.cleanup_result.objects) return std::nullopt;
    PinnedClosureFailure failure{Stage::Cleanup, data.cleanup_result.objects->reason, data.cleanup_result.objects->error_number};
    failure.cleanup = data.cleanup_result;
    failure.abandoned_root = data.root_path;
    return failure;
}

std::optional<PinnedClosureFailure> PinnedSubmoduleWorkspaceAuthority::clone_objects(
    const InvocationOwnedPinnedSubmoduleClosure& closure, std::size_t node,
    const fs::path& target, int parent_descriptor, Clock::time_point deadline,
    BoundedCapturedProcessResult (*runner)(const ExplicitProcessInvocation&, const BoundedProcessPolicy&)) {
    if(!closure.valid()) return PinnedClosureFailure{Stage::Input, Reason::InvalidSelection};
    auto& data = *closure.data_;
    try {
        // Human review does not extend the acquisition/read deadline. This
        // local transfer has its own phase budget and never writes the source.
        data.inspect(deadline);
        const auto& repo = data.repositories.at(node);
        auto arguments = trusted_git_recipe_acquisition_process_arguments();
        // Git checks protocol.file even for --local's direct object copy.
        // Only this private source/target-bound transfer enables it.
        arguments.insert(arguments.end(), {"-c", "protocol.https.allow=never", "-c", "protocol.file.allow=always"});
        arguments.insert(arguments.end(), {"clone", "--local", "--no-hardlinks", "--no-checkout", "--quiet", "--template=",
                                           "--", (data.root_path / repo.leaf).string(), target.string()});
        Descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if(input.get() < 0) fail(Stage::ObjectProof, Reason::IoFailure, errno);
        ExplicitProcessInvocation invocation{"/usr/bin/git", std::move(arguments),
                                             trusted_git_process_environment(TrustedGitProcessEnvironmentMode::ManagedOperation)};
        invocation.working_directory_fd = parent_descriptor;
        invocation.standard_input_fd = input.get();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if(remaining.count() <= 0) fail(Stage::ObjectProof, Reason::ResourceLimitExceeded);
        auto result = runner(invocation, BoundedProcessPolicy{remaining, std::chrono::milliseconds(200), 65536, false, true});
        const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
        if(result.cancellation_signal || !exited || exited->exit_code != 0) {
            PinnedClosureFailure failure{Stage::ObjectProof, result.cancellation_signal ? Reason::Cancelled : Reason::GitProcessFailed};
            failure.process = std::move(result);
            return failure;
        }
        data.inspect(deadline);
        return std::nullopt;
    } catch(Failure& error) {
        return std::move(error.detail);
    } catch(const std::bad_alloc&) {
        return PinnedClosureFailure{Stage::ObjectProof, Reason::ResourceLimitExceeded};
    } catch(const std::exception&) {
        return PinnedClosureFailure{Stage::ObjectProof, Reason::MalformedRepository};
    }
}

EvaluatedDevelSourceSelection& PinnedSubmoduleWorkspaceAuthority::selection(InvocationOwnedPinnedSubmoduleClosure& closure) {
    if(!closure.valid()) throw std::logic_error("Pinned closure is inactive");
    return closure.data_->selection;
}

InvocationOwnedPinnedSubmoduleClosure::InvocationOwnedPinnedSubmoduleClosure(std::unique_ptr<PinnedSubmoduleClosureData> data) noexcept : data_(std::move(data)) {
}
InvocationOwnedPinnedSubmoduleClosure::InvocationOwnedPinnedSubmoduleClosure(InvocationOwnedPinnedSubmoduleClosure&&) noexcept = default;
InvocationOwnedPinnedSubmoduleClosure::~InvocationOwnedPinnedSubmoduleClosure() noexcept = default;
bool InvocationOwnedPinnedSubmoduleClosure::valid() const noexcept {
    return data_ && !data_->closed && data_->selection.valid();
}
const EvaluatedDevelSourceSelection& InvocationOwnedPinnedSubmoduleClosure::selection() const {
    if(!valid()) throw std::logic_error("Pinned closure is closed");
    return data_->selection;
}
const std::vector<PinnedSubmoduleNode>& InvocationOwnedPinnedSubmoduleClosure::nodes() const {
    static_cast<void>(selection());
    return data_->nodes;
}
const std::vector<PinnedSubmoduleEdge>& InvocationOwnedPinnedSubmoduleClosure::edges() const {
    static_cast<void>(selection());
    return data_->edges;
}
PinnedRootTag::PinnedRootTag(std::string name, ReviewedSourceObjectId raw, std::optional<ReviewedSourceObjectId> peeled)
    : ref_name_(std::move(name)), raw_(std::move(raw)), peeled_(std::move(peeled)) {
}
const std::string& PinnedRootTag::ref_name() const noexcept {
    return ref_name_;
}
const ReviewedSourceObjectId& PinnedRootTag::raw() const noexcept {
    return raw_;
}
const std::optional<ReviewedSourceObjectId>& PinnedRootTag::peeled() const noexcept {
    return peeled_;
}
const std::vector<PinnedRootTag>& InvocationOwnedPinnedSubmoduleClosure::root_tags() const {
    static_cast<void>(selection());
    return data_->root_tags;
}
PinnedClosureCleanupResult InvocationOwnedPinnedSubmoduleClosure::cleanup() noexcept {
    return data_ ? data_->cleanup() : PinnedClosureCleanupResult{};
}
std::variant<std::string, PinnedClosureFailure> InvocationOwnedPinnedSubmoduleClosure::read_blob(std::size_t node, std::size_t entry) {
    if(!valid()) return PinnedClosureFailure{Stage::Input, Reason::InvalidSelection};
    try {
        data_->run(Stage::ObjectProof, node, {"fsck", "--strict", "--no-reflogs", "--no-dangling", data_->nodes.at(node).commit.value()}, 65536);
        return data_->blob(node, data_->nodes.at(node).inventory.entries.at(entry), MAX_BLOB_BYTES);
    } catch(Failure& error) {
        error.detail.cleanup = data_->cleanup();
        if(error.detail.cleanup.objects) error.detail.abandoned_root = data_->root_path;
        return std::move(error.detail);
    } catch(const std::exception&) {
        PinnedClosureFailure failure{Stage::Input, Reason::MalformedTree};
        failure.cleanup = data_->cleanup();
        if(failure.cleanup.objects) failure.abandoned_root = data_->root_path;
        return failure;
    }
}
PinnedSubmoduleClosureResult acquire_pinned_submodule_closure(EvaluatedDevelSourceSelection selection, PresentationDetail presentation_detail) {
    std::unique_ptr<PinnedSubmoduleClosureData> data;
    PinnedClosureFailure failure{Stage::Input, Reason::InvalidSelection};
    try {
        if(!selection.valid()) fail(Stage::Input, Reason::InvalidSelection);
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
        // Model allocation failure before the constructor can consume selection.
        if(std::exchange(g_hooks.fail_next_backing_allocation, false)) throw std::bad_alloc();
#endif
        data = std::make_unique<PinnedSubmoduleClosureData>(std::move(selection));
        data->create();
        const auto oid = data->observe();
        std::set<std::pair<std::string, std::string>> ancestry;
        data->traverse(data->selection.git_source().source_location(), oid, std::nullopt, 0, ancestry, presentation_detail);
        data->inspect();
        data->check();
        return InvocationOwnedPinnedSubmoduleClosure(std::move(data));
    } catch(Failure& error) {
        failure = std::move(error.detail);
    } catch(const std::bad_alloc&) {
        failure = {data ? data->active : Stage::Input, Reason::ResourceLimitExceeded};
    } catch(const std::exception&) {
        failure = {data ? data->active : Stage::Input, Reason::MalformedRepository};
    }
    if(data) {
        failure.cleanup = data->cleanup();
        if(failure.cleanup.objects) failure.abandoned_root = data->root_path;
    } else if(selection.valid()) {
        // Backing allocation can fail while the input still owns its context.
        const auto cleaned = selection.cleanup();
        if(const auto* consequence = std::get_if<InvocationOwnedSourceBuildContextFailure>(&cleaned)) failure.cleanup.selection = *consequence;
    }
    return failure;
}
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
void set_pinned_closure_test_hooks(PinnedClosureTestHooks hooks) {
    g_hooks = std::move(hooks);
}
#endif
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
std::optional<PinnedClosureFailure> check_pinned_declaration_fixture(std::string_view bytes) {
    try {
        static_cast<void>(declarations(bytes));
        return std::nullopt;
    } catch(Failure& error) {
        return std::move(error.detail);
    }
}
#endif
