#include "pinned_submodule_workspace.hpp"
#include "trusted_git_process_policy.hpp"
#include "logging.hpp"
#include "reviewed_source_git_parser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <linux/memfd.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Stage = PinnedWorkspaceStage;
using Reason = PinnedWorkspaceFailureReason;
constexpr auto PHASE_TIMEOUT = std::chrono::minutes(10);
constexpr auto CLEANUP_TIMEOUT = std::chrono::seconds(5);
constexpr std::size_t MAX_ENTRIES = 262144, MAX_DEPTH = 128, MAX_PROCESSES = 4096;
constexpr std::uintmax_t MAX_BYTES = 1024ULL * 1024 * 1024;
constexpr std::size_t MAX_CAPTURE = 1024 * 1024;
constexpr int TAG_TRANSACTION_SEALS = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
constexpr const char* WORKSPACE_LEAF = "pinned-submodule-workspace";
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
PinnedWorkspaceTestHooks g_hooks;
#endif

class Failure final : public std::exception {
public:
    PinnedWorkspaceFailure detail;
    explicit Failure(PinnedWorkspaceFailure value) : detail(std::move(value)) {
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
struct stat status(int fd, Stage stage) {
    struct stat value{};
    if(::fstat(fd, &value) != 0) fail(stage, Reason::UnsafeFilesystem, errno);
    return value;
}
bool same_identity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_uid == b.st_uid && a.st_mode == b.st_mode &&
           (!S_ISREG(a.st_mode) || a.st_nlink == b.st_nlink);
}
Descriptor open_beneath(int parent, const fs::path& path, bool directory, Stage stage) {
    open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (directory ? O_DIRECTORY : 0);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    const int fd = static_cast<int>(::syscall(SYS_openat2, parent, path.c_str(), &how, sizeof(how)));
    if(fd < 0) fail(stage, Reason::UnsafeFilesystem, errno);
    return Descriptor(fd);
}
struct Retained {
    fs::path path;
    Descriptor descriptor;
    struct stat identity;
};
Retained retain(int root, const fs::path& path, bool directory, Stage stage) {
    auto fd = open_beneath(root, path, directory, stage);
    auto identity = status(fd.get(), stage);
    if((directory ? !S_ISDIR(identity.st_mode) : !S_ISREG(identity.st_mode)) || identity.st_uid != ::geteuid() ||
       (identity.st_mode & 0022) || (!directory && identity.st_nlink != 1)) fail(stage, Reason::UnsafeFilesystem);
    return {path, std::move(fd), identity};
}
void require_retained(int root, const Retained& retained, Stage stage) {
    auto current = open_beneath(root, retained.path, S_ISDIR(retained.identity.st_mode), stage);
    if(!same_identity(retained.identity, status(current.get(), stage)) ||
       !same_identity(retained.identity, status(retained.descriptor.get(), stage))) fail(stage, Reason::UnsafeFilesystem);
}
std::string read_regular(int root, const fs::path& path, Stage stage, std::size_t limit = MAX_CAPTURE) {
    auto file = retain(root, path, false, stage);
    if(file.identity.st_size < 0 || static_cast<std::uintmax_t>(file.identity.st_size) > limit) fail(stage, Reason::ResourceLimitExceeded);
    std::string bytes;
    std::array<char, 4096> buffer{};
    while(true) {
        const auto count = ::read(file.descriptor.get(), buffer.data(), buffer.size());
        if(count < 0 && errno == EINTR) continue;
        if(count < 0) fail(stage, Reason::UnsafeFilesystem, errno);
        if(count == 0) break;
        if(static_cast<std::size_t>(count) > limit - bytes.size()) fail(stage, Reason::ResourceLimitExceeded);
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    require_retained(root, file, stage);
    return bytes;
}
using Inventory = std::map<fs::path, struct stat>;
struct ScanBudget {
    Clock::time_point deadline;
    std::uintmax_t bytes = 0;
};
struct DirectoryCloser {
    void operator()(DIR* value) const noexcept {
        ::closedir(value);
    }
};
void scan(int root, const fs::path& prefix, dev_t device, std::size_t depth, Stage stage, ScanBudget& budget, Inventory& result, bool mutable_content = false) {
    if(depth > MAX_DEPTH || Clock::now() >= budget.deadline) fail(stage, Reason::ResourceLimitExceeded);
    auto fd = open_beneath(root, prefix.empty() ? fs::path(".") : prefix, true, stage);
    const int scan_fd = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
    if(scan_fd < 0) fail(stage, Reason::UnsafeFilesystem, errno);
    std::unique_ptr<DIR, DirectoryCloser> stream(::fdopendir(scan_fd));
    if(!stream) {
        ::close(scan_fd);
        fail(stage, Reason::UnsafeFilesystem, errno);
    }
    while(true) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if(!entry) {
            if(errno) fail(stage, Reason::UnsafeFilesystem, errno);
            break;
        }
        const std::string name(entry->d_name);
        if(name == "." || name == "..") continue;
        if(result.size() >= MAX_ENTRIES || Clock::now() >= budget.deadline) fail(stage, Reason::ResourceLimitExceeded);
        const auto path = prefix / name;
        struct stat identity{};
        if(::fstatat(fd.get(), name.c_str(), &identity, AT_SYMLINK_NOFOLLOW) != 0) fail(stage, Reason::UnsafeFilesystem, errno);
        if((!S_ISDIR(identity.st_mode) && !S_ISREG(identity.st_mode) && !(mutable_content && S_ISLNK(identity.st_mode))) || identity.st_dev != device ||
           identity.st_uid != ::geteuid() || (!mutable_content && ((identity.st_mode & 0022) || (S_ISREG(identity.st_mode) && identity.st_nlink != 1))))
            fail(stage, Reason::UnsafeFilesystem);
        if(S_ISREG(identity.st_mode)) {
            if(identity.st_size < 0 || static_cast<std::uintmax_t>(identity.st_size) > MAX_BYTES - budget.bytes) fail(stage, Reason::ResourceLimitExceeded);
            budget.bytes += static_cast<std::uintmax_t>(identity.st_size);
        }
        result.emplace(path, identity);
        if(S_ISDIR(identity.st_mode)) scan(root, path, device, depth + 1, stage, budget, result, mutable_content);
    }
}
bool within(const fs::path& path, const fs::path& parent) {
    const auto relative = path.lexically_relative(parent);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
BoundedCapturedProcessResult execute_git(const ExplicitProcessInvocation& invocation, const BoundedProcessPolicy& policy) {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
    if(g_hooks.process) return g_hooks.process(invocation, policy);
#endif
    return capture_bounded_explicit_process_output_raw(invocation, policy);
}
struct Binding {
    Retained worktree, gitdir, objects;
    std::optional<Retained> gitfile;
    std::string config;
    std::optional<std::string> declaration;
};
} // namespace

// No construction friendship: the whole Accepted owner remains the authority.
struct PinnedSubmoduleWorkspaceData {
    AcceptedPinnedSubmoduleClosure accepted;
    fs::path root_path;
    Descriptor parent;
    std::optional<Retained> root;
    std::vector<Binding> bindings;
    Inventory cleanup_inventory;
    std::optional<Retained> native_base, native_src, mirror, mirror_objects;
    std::string mirror_config;
    Descriptor build_parent, source_parent;
    bool native_prepared = false, execution_started = false;
    bool inventory_sealed = false, created = false, closed = false, ready = false, refuse_cleanup = false;
    PinnedWorkspaceCleanupResult cleanup_result;
    Stage active = Stage::Input;
    Clock::time_point deadline = Clock::now() + PHASE_TIMEOUT;
    std::size_t processes = 0;

    explicit PinnedSubmoduleWorkspaceData(AcceptedPinnedSubmoduleClosure value) noexcept : accepted(std::move(value)) {
    }
    void check() const {
        if(Clock::now() >= deadline || processes >= MAX_PROCESSES) fail(active, Reason::ResourceLimitExceeded);
    }
    Inventory inventory(Clock::time_point end) const {
        Inventory result;
        if(!root) fail(active, Reason::UnsafeFilesystem);
        require_retained(parent.get(), *root, active);
        if(native_base) require_retained(build_parent.get(), *native_base, active);
        if(native_src) require_retained(native_base->descriptor.get(), *native_src, active);
        if(mirror) require_retained(source_parent.get(), *mirror, active);
        if(mirror_objects) require_retained(mirror->descriptor.get(), *mirror_objects, active);
        ScanBudget budget{end};
        scan(root->descriptor.get(), {}, root->identity.st_dev, 0, active, budget, result, execution_started);
        return result;
    }
    std::string run(int cwd, std::vector<std::string> operation, std::size_t limit = MAX_CAPTURE, int input_fd = -1, bool diagnostics = false) {
        check();
        ++processes;
        auto arguments = trusted_git_recipe_acquisition_process_arguments();
        // No transport is used here. Keep every protocol disabled, including
        // HTTPS; --local clone in the private transfer copies files directly.
        arguments.insert(arguments.end(), {"-c", "protocol.https.allow=never"});
        arguments.insert(arguments.end(), operation.begin(), operation.end());
        Descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if(input.get() < 0) fail(active, Reason::UnsafeFilesystem, errno);
        ExplicitProcessInvocation invocation{"/usr/bin/git", std::move(arguments), trusted_git_process_environment(TrustedGitProcessEnvironmentMode::ReadOnlyObservation)};
        invocation.working_directory_fd = cwd;
        invocation.standard_input_fd = input_fd < 0 ? input.get() : input_fd;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if(remaining.count() <= 0) fail(active, Reason::ResourceLimitExceeded);
        auto result = execute_git(invocation, {remaining, std::chrono::milliseconds(200), limit, true, diagnostics});
        const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
        if(result.cancellation_signal || !exited || exited->exit_code != 0) {
            PinnedWorkspaceFailure failure{active, result.cancellation_signal ? Reason::Cancelled : Reason::GitProcessFailed};
            failure.process = std::move(result);
            throw Failure(std::move(failure));
        }
        return std::move(result.output);
    }
    void project_root_tags() {
        active = Stage::RootMaterialization;
        std::string transaction;
        for(const auto& tag : accepted.closure().root_tags()) {
            transaction += "create " + tag.ref_name();
            transaction.push_back('\0');
            transaction += tag.raw().value();
            transaction.push_back('\0');
        }
        if(transaction.empty()) return;
        Descriptor input(static_cast<int>(::syscall(SYS_memfd_create, "moguet-root-tags", MFD_CLOEXEC | MFD_ALLOW_SEALING)));
        if(input.get() < 0) fail(active, Reason::UnsafeFilesystem, errno);
        for(std::size_t written = 0; written < transaction.size();) {
            const auto count = ::write(input.get(), transaction.data() + written, transaction.size() - written);
            if(count < 0 && errno == EINTR) continue;
            if(count <= 0) fail(active, Reason::UnsafeFilesystem, errno);
            written += static_cast<std::size_t>(count);
        }
        int sealed;
        do {
            sealed = ::fcntl(input.get(), F_ADD_SEALS, TAG_TRANSACTION_SEALS);
        } while(sealed < 0 && errno == EINTR);
        if(sealed < 0) fail(active, Reason::UnsafeFilesystem, errno);
        int seals;
        do {
            seals = ::fcntl(input.get(), F_GET_SEALS);
        } while(seals < 0 && errno == EINTR);
        if(seals < 0) fail(active, Reason::UnsafeFilesystem, errno);
        if((seals & TAG_TRANSACTION_SEALS) != TAG_TRANSACTION_SEALS) fail(active, Reason::UnsafeFilesystem);
        // Reprove the immutable cross-process input against the typed mapping,
        // including its exact length; sealing alone cannot prove copied bytes.
        const auto metadata = status(input.get(), active);
        if(metadata.st_size < 0 || static_cast<std::uintmax_t>(metadata.st_size) != transaction.size())
            fail(active, Reason::UnsafeFilesystem);
        std::array<char, 4096> buffer{};
        for(std::size_t offset = 0; offset < transaction.size();) {
            const auto count = ::pread(input.get(), buffer.data(), std::min(buffer.size(), transaction.size() - offset), static_cast<off_t>(offset));
            if(count < 0 && errno == EINTR) continue;
            if(count < 0) fail(active, Reason::UnsafeFilesystem, errno);
            if(count == 0 || !std::equal(buffer.begin(), buffer.begin() + count, transaction.begin() + offset))
                fail(active, Reason::UnsafeFilesystem);
            offset += static_cast<std::size_t>(count);
        }
        ssize_t extra;
        do {
            extra = ::pread(input.get(), buffer.data(), 1, static_cast<off_t>(transaction.size()));
        } while(extra < 0 && errno == EINTR);
        if(extra < 0) fail(active, Reason::UnsafeFilesystem, errno);
        if(extra != 0) fail(active, Reason::UnsafeFilesystem);
        if(::lseek(input.get(), 0, SEEK_SET) != 0) fail(active, Reason::UnsafeFilesystem, errno);
        // One create-only transaction: no dereferencing, replacement, partial
        // batch success, or mutation of the accepted object-only backing.
        try {
            run(root->descriptor.get(), {"update-ref", "--no-deref", "--stdin", "-z"}, MAX_CAPTURE, input.get());
        } catch(const Failure&) {
            // A failed create may expose preexisting, unproved tag metadata
            // before seal/prove. Preserve the primary process/cancel failure
            // without letting cleanup adopt that namespace as our own.
            refuse_cleanup = true;
            throw;
        }
    }
    void prove_tags(int cwd) {
        bool namespace_proven = false;
        try {
            std::string expected;
            for(const auto& tag : accepted.closure().root_tags()) {
                expected += tag.ref_name();
                expected.push_back('\0');
                expected += tag.raw().value();
                expected.append("\0\0\n", 3); // a symbolic tag never matches
            }
            // Include diagnostics: Git may warn and omit malformed refs.
            if(run(cwd, {"for-each-ref", "--sort=refname", "--format=%(refname)%00%(objectname)%00%(symref)%00", "refs/tags/"}, MAX_CAPTURE, -1, true) != expected)
                fail(active, Reason::TagNamespaceDrift);
            namespace_proven = true;
            const auto format = accepted.closure().nodes().front().commit.format();
            if(run(cwd, {"rev-parse", "--show-object-format=storage"}) != (format == GitObjectFormat::Sha256 ? "sha256\n" : "sha1\n"))
                fail(active, Reason::TagObjectInvalid);
            // Do not pass explicit object roots here: fsck must walk the ref
            // namespace too, including dangling symbolic refs omitted by both
            // for-each-ref and refs verify. The exact logical mapping above
            // makes all accepted raw tags roots of this hash/connectivity proof.
            run(cwd, {"fsck", "--strict", "--no-reflogs", "--no-dangling"});
        } catch(Failure& error) {
            // An unproved root/mirror tag store may contain unknown metadata
            // subtrees. Keep the primary semantic/process failure, but never
            // let generic context cleanup adopt that state as generated content.
            refuse_cleanup = true;
            if(error.detail.reason == Reason::GitProcessFailed && error.detail.process &&
               std::holds_alternative<BoundedProcessExited>(error.detail.process->outcome))
                error.detail.reason = namespace_proven ? Reason::TagObjectInvalid : Reason::TagNamespaceDrift;
            throw;
        } catch(...) {
            refuse_cleanup = true;
            throw;
        }
    }
    void bind(std::size_t node, const fs::path& worktree_path, const fs::path& gitdir_path) {
        active = Stage::GitdirBinding;
        const int fd = root->descriptor.get();
        Binding binding{retain(fd, worktree_path, true, active), retain(fd, gitdir_path, true, active), retain(fd, gitdir_path / "objects", true, active), std::nullopt, {}, std::nullopt};
        if(node != 0) binding.gitfile.emplace(retain(fd, worktree_path / ".git", false, active));
        bindings.push_back(std::move(binding));
    }
    void seal() {
        active = Stage::SourceReadyReproof;
        for(std::size_t i = 0; i < bindings.size(); ++i) {
            auto& binding = bindings[i];
            binding.config = read_regular(binding.gitdir.descriptor.get(), "config", active);
            for(const auto& entry : accepted.closure().nodes()[i].inventory.entries) {
                if(entry.path().raw_bytes() == ".gitmodules") {
                    binding.declaration = run(binding.worktree.descriptor.get(), {"cat-file", "blob", entry.object_id().value()});
                }
            }
        }
        cleanup_inventory = inventory(deadline);
        inventory_sealed = true;
    }
    void prove(Stage stage = Stage::SourceReadyReproof) {
        active = stage;
        const bool clean = stage == Stage::SourceReadyReproof || stage == Stage::NativePreparation;
        check();
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
        if(g_hooks.before_reproof) g_hooks.before_reproof(root_path);
#endif
        const auto entries = inventory(deadline);
        const auto& nodes = accepted.closure().nodes();
        const auto& edges = accepted.closure().edges();
        if(bindings.size() != nodes.size()) fail(active, Reason::MissingModule);
        const int fd = root->descriptor.get();
        std::set<fs::path> gitfiles{fs::path(".git")};
        for(const auto& binding : bindings)
            if(binding.gitfile) gitfiles.insert(binding.gitfile->path);
        for(const auto& [path, identity] : entries) {
            static_cast<void>(identity);
            if(path.filename() == ".git" && !gitfiles.contains(path)) fail(active, Reason::UnexpectedModule);
        }
        if(native_src) {
            Inventory sources;
            ScanBudget budget{deadline};
            scan(native_src->descriptor.get(), {}, root->identity.st_dev, 0, active, budget, sources, execution_started);
            for(const auto& [path, identity] : sources) {
                static_cast<void>(identity);
                if(path.filename() == ".git" && !gitfiles.contains(path.lexically_relative(root->path))) fail(active, Reason::UnexpectedModule);
            }
        }
        // Namespace directories lead to exactly the expected direct children.
        // Stop at a child gitdir; its nested modules are checked as a parent
        // in the next iteration, using its own logical names.
        for(std::size_t i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings[i];
            std::vector<fs::path> expected;
            for(const auto& edge : edges)
                if(edge.parent == i) expected.push_back(bindings[edge.child].gitdir.path);
            const auto modules = binding.gitdir.path / "modules";
            for(const auto& [path, identity] : entries) {
                if(!within(path, modules)) continue;
                if(path == modules) {
                    if(expected.empty()) fail(active, Reason::UnexpectedModule);
                    continue;
                }
                bool allowed = false;
                for(const auto& child : expected) {
                    if(within(path, child) || (S_ISDIR(identity.st_mode) && within(child, path))) allowed = true;
                }
                if(!allowed) fail(active, Reason::UnexpectedModule);
            }
            if(!entries.contains(binding.gitdir.path) || !entries.contains(binding.worktree.path == "." ? fs::path(".git") : binding.worktree.path))
                fail(active, Reason::MissingModule);
            require_retained(fd, binding.worktree, active);
            require_retained(fd, binding.gitdir, active);
            require_retained(fd, binding.objects, active);
            if(binding.gitfile) {
                if(!entries.contains(binding.gitfile->path)) fail(active, Reason::MissingModule);
                require_retained(fd, *binding.gitfile, active);
                // Canonical native spelling, including the required '..'. No
                // generic gitfile parser and no acceptance of absolute targets.
                const auto relative = binding.gitdir.path.lexically_relative(binding.worktree.path);
                const auto expected_file = "gitdir: " + relative.generic_string() + "\n";
                if(read_regular(fd, binding.gitfile->path, active) != expected_file) fail(active, Reason::GitfileMismatch);
                if((binding.worktree.path / relative).lexically_normal() != binding.gitdir.path) fail(active, Reason::GitdirMismatch);
            }
            if(read_regular(binding.gitdir.descriptor.get(), "config", active) != binding.config) fail(active, Reason::GitdirMismatch);
            // Reject external backing, not Git's derived indexes. Native
            // makepkg extraction runs git fetch, whose maintenance can write
            // objects/info/commit-graph(s) even when the accepted OID is unchanged.
            for(const auto& [path, identity] : entries) {
                if(S_ISREG(identity.st_mode) && (path == binding.gitdir.path / "objects/info/alternates" ||
                                                 path == binding.gitdir.path / "objects/info/http-alternates" ||
                                                 path == binding.gitdir.path / "commondir" || path == binding.gitdir.path / "shallow" ||
                                                 path == binding.gitdir.path / "info/grafts" || within(path, binding.gitdir.path / "refs/replace") ||
                                                 (within(path, binding.gitdir.path / "objects/pack") && path.extension() == ".promisor"))) fail(active, Reason::GitdirMismatch);
            }
        }
        prove_tags(bindings.front().worktree.descriptor.get());
        if(mirror) {
            if(read_regular(mirror->descriptor.get(), "config", active) != mirror_config) fail(active, Reason::GitdirMismatch);
            prove_tags(mirror->descriptor.get());
        }
        // Validate every binding before Git status can descend into children.
        for(std::size_t i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings[i];
            const int cwd = binding.worktree.descriptor.get();
            if(run(cwd, {"rev-parse", "--absolute-git-dir"}) != (root_path / binding.gitdir.path).string() + "\n" ||
               run(cwd, {"rev-parse", "--show-toplevel"}) != (i == 0 ? root_path : root_path / binding.worktree.path).string() + "\n") fail(active, Reason::GitdirMismatch);
            if(i != 0) {
                const auto back = binding.worktree.path.lexically_relative(binding.gitdir.path).generic_string();
                if(run(cwd, {"config", "--local", "--get", "core.worktree"}) != back + "\n") fail(active, Reason::GitdirMismatch);
            }
            const auto& node = nodes[i];
            if(run(cwd, {"rev-parse", "--verify", "HEAD"}) != node.commit.value() + "\n" ||
               run(cwd, {"cat-file", "-t", node.commit.value()}) != "commit\n" ||
               run(cwd, {"rev-parse", "--show-object-format=storage"}) != (node.commit.format() == GitObjectFormat::Sha256 ? "sha256\n" : "sha1\n")) fail(active, Reason::RevisionDrift);
            run(cwd, {"fsck", "--strict", "--no-reflogs", "--no-dangling", node.commit.value()});
            // Exact tree equality includes every index mode160000 path/pin,
            // rather than accepting child HEAD alone as parent gitlink proof.
            if(clean) {
                if(run(cwd, {"write-tree"}) != node.tree.value() + "\n") fail(active, Reason::RevisionDrift);
            } else {
                // Ordinary staged content may change. Only the complete index
                // gitlink set is revision authority at prepared/build points.
                const auto index = run(cwd, {"ls-files", "--stage", "-z"});
                std::map<std::string, std::string> links;
                bool declaration_index = false;
                std::size_t begin = 0;
                while(begin < index.size()) {
                    const auto end = index.find('\0', begin);
                    const auto tab = index.find('\t', begin);
                    if(end == std::string::npos || tab == std::string::npos || tab >= end) fail(active, Reason::RevisionDrift);
                    const auto header = index.substr(begin, tab - begin);
                    const auto path = index.substr(tab + 1, end - tab - 1);
                    if(path == ".gitmodules") {
                        const auto found = std::find_if(node.inventory.entries.begin(), node.inventory.entries.end(), [](const auto& entry) {
                            return entry.path().raw_bytes() == ".gitmodules";
                        });
                        if(found == node.inventory.entries.end() || declaration_index) fail(active, Reason::DeclarationDrift);
                        const std::string mode = found->mode() == ReviewedSourceFileMode::Executable ? "100755 " : "100644 ";
                        if(header != mode + found->object_id().value() + " 0") fail(active, Reason::DeclarationDrift);
                        declaration_index = true;
                    }
                    if(header.starts_with("160000 ")) {
                        if(!header.ends_with(" 0") || !links.emplace(index.substr(tab + 1, end - tab - 1), header.substr(7, header.size() - 9)).second)
                            fail(active, Reason::RevisionDrift);
                    }
                    begin = end + 1;
                }
                if(declaration_index != binding.declaration.has_value()) fail(active, Reason::DeclarationDrift);
                std::map<std::string, std::string> expected;
                for(const auto& edge : edges)
                    if(edge.parent == i) expected.emplace(edge.path, edge.pin.value());
                if(links != expected) fail(active, Reason::RevisionDrift);
            }
            const auto declaration_path = (binding.worktree.path / ".gitmodules").lexically_normal();
            if(binding.declaration) {
                if(!entries.contains(declaration_path) || read_regular(fd, declaration_path, active) != *binding.declaration) fail(active, Reason::DeclarationDrift);
            } else if(entries.contains(declaration_path))
                fail(active, Reason::DeclarationDrift);
            if(clean && !run(cwd, {"status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching", "--ignore-submodules=none"}).empty()) fail(active, Reason::RevisionDrift);
        }
        if(mirror && read_regular(mirror->descriptor.get(), "config", active) != mirror_config) fail(active, Reason::GitdirMismatch);
        check();
    }
};

BoundedCapturedProcessResult PinnedSubmoduleWorkspaceAuthority::execute(const ExplicitProcessInvocation& invocation, const BoundedProcessPolicy& policy) {
    return execute_git(invocation, policy);
}

EvaluatedDevelSourceSelection& PinnedSubmoduleWorkspaceAuthority::selection(SourceReadyPinnedSubmoduleWorkspace& workspace) {
    if(!workspace.valid()) throw std::logic_error("Pinned workspace is inactive");
    return selection(workspace.data_->accepted.closure_);
}

std::optional<PinnedWorkspaceFailure> PinnedSubmoduleWorkspaceAuthority::prepare_native(SourceReadyPinnedSubmoduleWorkspace& workspace) {
    if(!workspace.valid()) return PinnedWorkspaceFailure{Stage::Input, Reason::InvalidAcceptedClosure};
    auto& data = *workspace.data_;
    try {
        data.deadline = Clock::now() + PHASE_TIMEOUT;
        data.processes = 0;
        if(data.native_prepared) fail(Stage::NativePreparation, Reason::InvalidAcceptedClosure);
        data.prove();
        data.active = Stage::NativePreparation;
        auto& selected = selection(workspace);
        const auto& ctx = context(selected);
        const auto& source = selected.source_declaration().parsed_source;
        std::string name;
        if(source.destination_name)
            name = *source.destination_name;
        else {
            auto location = source.source_location;
            if(location.ends_with('/')) location.pop_back();
            name = location.substr(location.rfind('/') + 1);
            const auto suffix = name.find(".git");
            if(suffix != std::string::npos) name.erase(suffix);
        }
        if(name.empty() || name == "." || name == ".." || name == ".git" || fs::path(name).filename() != name)
            fail(data.active, Reason::UnsafeFilesystem);
        data.build_parent = Descriptor(::fcntl(builddir_descriptor(selected), F_DUPFD_CLOEXEC, 3));
        data.source_parent = Descriptor(::fcntl(srcdest_descriptor(selected), F_DUPFD_CLOEXEC, 3));
        if(data.build_parent.get() < 0 || data.source_parent.get() < 0) fail(data.active, Reason::UnsafeFilesystem, errno);
        Inventory source_inventory;
        ScanBudget budget{data.deadline};
        scan(data.source_parent.get(), {}, data.root->identity.st_dev, 0, data.active, budget, source_inventory);
        if(!source_inventory.empty()) {
            data.refuse_cleanup = true;
            fail(data.active, Reason::UnexpectedModule);
        }
        // Native makepkg uses BUILDDIR/pkgbase/src when startdir is the
        // separate working recipe. Relocate the retained closure once, never
        // rematerialize it; all relative child bindings remain unchanged.
        const auto base = ctx.package_base().package_base();
        if(::mkdirat(data.build_parent.get(), base.c_str(), 0700) != 0) {
            data.refuse_cleanup = true;
            fail(data.active, Reason::UnsafeFilesystem, errno);
        }
        data.native_base.emplace(retain(data.build_parent.get(), base, true, data.active));
        if(::mkdirat(data.native_base->descriptor.get(), "src", 0700) != 0) fail(data.active, Reason::UnsafeFilesystem, errno);
        data.native_src.emplace(retain(data.native_base->descriptor.get(), "src", true, data.active));
        const auto relocated = ctx.builddir() / base / "src" / name;
        Descriptor next_parent(::fcntl(data.native_src->descriptor.get(), F_DUPFD_CLOEXEC, 3));
        if(next_parent.get() < 0) fail(data.active, Reason::UnsafeFilesystem, errno);
        if(::renameat2(data.parent.get(), data.root->path.c_str(), next_parent.get(), name.c_str(), RENAME_NOREPLACE) != 0)
            fail(data.active, Reason::UnsafeFilesystem, errno);
        data.root->path = name;
        data.parent = std::move(next_parent);
        data.root_path = relocated;
        // SourceReady's independent object store supplies a disposable native
        // mirror. --holdver skips its remote download; extraction fetches only
        // this private mirror. No accepted backing or remote is reobserved.
        const auto mirror_path = ctx.srcdest() / name;
        data.run(data.source_parent.get(), {"-c", "protocol.file.allow=always", "clone", "--mirror", "--local", "--no-hardlinks", "--template=", "--", data.root_path.string(), name});
        data.mirror.emplace(retain(data.source_parent.get(), name, true, data.active));
        data.mirror_objects.emplace(retain(data.mirror->descriptor.get(), "objects", true, data.active));
        const auto& node = data.accepted.closure().nodes().front();
        const auto& selector = selected.git_source().selector();
        const std::string branch = selector.kind() == VcsSelectorKind::Branch ? *selector.value() : "moguet-pinned";
        data.run(data.mirror->descriptor.get(), {"update-ref", "refs/heads/" + branch, node.commit.value()});
        data.run(data.mirror->descriptor.get(), {"symbolic-ref", "HEAD", "refs/heads/" + branch});
        data.run(data.mirror->descriptor.get(), {"remote", "set-url", "origin", node.locator});
        data.run(data.root->descriptor.get(), {"remote", "set-url", "origin", mirror_path.string()});
        // The derived symbolic HEAD must already have its exact local target
        // before ref-aware fsck, rather than waiting for makepkg extraction.
        data.run(data.root->descriptor.get(), {"update-ref", "refs/remotes/origin/" + branch, node.commit.value()});
        data.run(data.root->descriptor.get(), {"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/" + branch});
        data.bindings.front().config = read_regular(data.bindings.front().gitdir.descriptor.get(), "config", data.active);
        data.mirror_config = read_regular(data.mirror->descriptor.get(), "config", data.active);
        data.cleanup_inventory = data.inventory(data.deadline);
        data.prove(Stage::NativePreparation);
        data.native_prepared = true;
        data.execution_started = true;
        return std::nullopt;
    } catch(Failure& error) {
        return std::move(error.detail);
    } catch(const std::bad_alloc&) {
        return PinnedWorkspaceFailure{Stage::NativePreparation, Reason::ResourceLimitExceeded};
    } catch(const std::exception&) {
        return PinnedWorkspaceFailure{Stage::NativePreparation, Reason::MaterializationFailed};
    }
}

std::optional<PinnedWorkspaceFailure> PinnedSubmoduleWorkspaceAuthority::reprove_execution(SourceReadyPinnedSubmoduleWorkspace& workspace, Stage stage) {
    if(!workspace.valid() || !workspace.data_->native_prepared || (stage != Stage::PreparedReproof && stage != Stage::PostBuildReproof))
        return PinnedWorkspaceFailure{stage, Reason::InvalidAcceptedClosure};
    try {
        workspace.data_->deadline = Clock::now() + PHASE_TIMEOUT;
        workspace.data_->processes = 0;
        workspace.data_->prove(stage);
        return std::nullopt;
    } catch(Failure& error) {
        // Newly observed repository/module metadata is not ordinary generated
        // content. Do not let context cleanup adopt that unexpected subtree.
        if(error.detail.reason == Reason::UnexpectedModule) workspace.data_->refuse_cleanup = true;
        return std::move(error.detail);
    } catch(const std::bad_alloc&) {
        return PinnedWorkspaceFailure{stage, Reason::ResourceLimitExceeded};
    } catch(const std::exception&) {
        return PinnedWorkspaceFailure{stage, Reason::MaterializationFailed};
    }
}

std::variant<PinnedSubmoduleWorkspaceAuthority::NativeMirror, PinnedWorkspaceFailure>
PinnedSubmoduleWorkspaceAuthority::prepared_native_mirror(SourceReadyPinnedSubmoduleWorkspace& workspace) {
    if(auto failure = reprove_execution(workspace, Stage::PreparedReproof)) return std::move(*failure);
    const auto& mirror = *workspace.data_->mirror;
    return NativeMirror{mirror.path, mirror.descriptor.get()};
}

PinnedWorkspaceCleanupResult PinnedSubmoduleWorkspaceAuthority::cleanup(PinnedSubmoduleWorkspaceData& data) noexcept {
    if(data.closed) return data.cleanup_result;
    data.closed = true;
    data.ready = false;
    data.active = Stage::Cleanup;
    try {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
        if(g_hooks.before_cleanup) g_hooks.before_cleanup(data.root_path);
#endif
        if(data.refuse_cleanup || (data.created && !data.root)) fail(Stage::Cleanup, Reason::UnsafeFilesystem);
        if(data.created) {
            for(const auto& binding : data.bindings) {
                const int fd = data.root->descriptor.get();
                require_retained(fd, binding.worktree, Stage::Cleanup);
                require_retained(fd, binding.gitdir, Stage::Cleanup);
                require_retained(fd, binding.objects, Stage::Cleanup);
                if(binding.gitfile) require_retained(fd, *binding.gitfile, Stage::Cleanup);
            }
            const auto current = data.inventory(Clock::now() + CLEANUP_TIMEOUT);
            if(data.inventory_sealed && !data.execution_started) {
                if(current.size() != data.cleanup_inventory.size()) fail(Stage::Cleanup, Reason::UnsafeFilesystem);
                for(const auto& [path, identity] : data.cleanup_inventory) {
                    const auto found = current.find(path);
                    if(found == current.end() || !same_identity(identity, found->second)) fail(Stage::Cleanup, Reason::UnsafeFilesystem);
                }
            }
        }
    } catch(const Failure& failure) {
        data.cleanup_result.workspace = PinnedWorkspaceCleanupFailure{failure.detail.reason, failure.detail.error_number};
    } catch(...) {
        data.cleanup_result.workspace = PinnedWorkspaceCleanupFailure{Reason::UnsafeFilesystem, std::nullopt};
    }
    if(data.cleanup_result.workspace && data.accepted.valid()) refuse_context_cleanup(data.accepted.closure().selection());
    // Delegate the actual bounded removal to the existing invocation owner.
    // Refusal above prevents its generic scan from adopting replaced paths.
    if(data.accepted.valid()) data.cleanup_result.closure = data.accepted.cleanup();
    return data.cleanup_result;
}
PinnedSubmoduleWorkspaceResult PinnedSubmoduleWorkspaceAuthority::materialize(AcceptedPinnedSubmoduleClosure accepted) {
    std::unique_ptr<PinnedSubmoduleWorkspaceData> data;
    PinnedWorkspaceFailure failure{Stage::Input, Reason::InvalidAcceptedClosure};
    try {
        if(!accepted.valid()) fail(Stage::Input, Reason::InvalidAcceptedClosure);
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
        if(std::exchange(g_hooks.fail_next_allocation, false)) throw std::bad_alloc();
#endif
        data = std::make_unique<PinnedSubmoduleWorkspaceData>(std::move(accepted));
        const auto& closure = data->accepted.closure();
        const auto& context_value = context(closure.selection());
        const auto validation = context_value.revalidate();
        if(const auto* error = std::get_if<InvocationOwnedSourceBuildContextFailure>(&validation)) {
            failure.context = *error;
            throw Failure(std::move(failure));
        }
        const auto& nodes = closure.nodes();
        const auto& edges = closure.edges();
        if(nodes.empty() || nodes.size() != edges.size() + 1) fail(Stage::Input, Reason::InvalidAcceptedClosure);
        std::vector<fs::path> worktrees{fs::path(".")}, gitdirs{fs::path(".git")};
        // Parent-first occurrence inventory; names may contain '/', but a
        // sibling's namespace must not overlap another repository's metadata.
        for(std::size_t i = 1; i < nodes.size(); ++i) {
            if(!nodes[i].parent_edge || *nodes[i].parent_edge >= edges.size()) fail(Stage::Input, Reason::InvalidAcceptedClosure);
            const auto& edge = edges[*nodes[i].parent_edge];
            if(edge.child != i || edge.parent >= i || edge.pin != nodes[i].commit) fail(Stage::Input, Reason::InvalidAcceptedClosure);
            for(const auto& other : edges)
                if(&other != &edge && other.parent == edge.parent &&
                   (within(edge.logical_name, other.logical_name) || within(other.logical_name, edge.logical_name))) fail(Stage::Input, Reason::GitdirMismatch);
            worktrees.push_back((worktrees[edge.parent] / edge.path).lexically_normal());
            gitdirs.push_back(gitdirs[edge.parent] / "modules" / edge.logical_name);
        }
        data->root_path = context_value.builddir() / WORKSPACE_LEAF;
        data->parent = Descriptor(::fcntl(builddir_descriptor(closure.selection()), F_DUPFD_CLOEXEC, 3));
        if(data->parent.get() < 0) fail(Stage::RootMaterialization, Reason::UnsafeFilesystem, errno);
        data->active = Stage::RootMaterialization;
        if(::mkdirat(data->parent.get(), WORKSPACE_LEAF, 0700) != 0) {
            // A deterministic name is not ownership of preexisting content.
            data->refuse_cleanup = true;
            fail(data->active, Reason::UnsafeFilesystem, errno);
        }
        data->created = true;
        data->root.emplace(retain(data->parent.get(), WORKSPACE_LEAF, true, data->active));
        for(std::size_t i = 0; i < nodes.size(); ++i) {
            data->active = Stage::ObjectTransfer;
            data->check();
            ++data->processes;
            Logger::raw_cmd("git clone --local --no-hardlinks --no-checkout [accepted node " + std::to_string(i) + "]");
            if(auto error = clone_objects(closure, i, worktrees[i], data->root->descriptor.get(), data->deadline, execute)) {
                PinnedWorkspaceFailure transfer{Stage::ObjectTransfer, error->reason == PinnedClosureFailureReason::Cancelled ? Reason::Cancelled : error->reason == PinnedClosureFailureReason::ResourceLimitExceeded ? Reason::ResourceLimitExceeded
                                                                                                                                                                                                                       : Reason::MaterializationFailed};
                transfer.acquisition = std::move(error);
                throw Failure(std::move(transfer));
            }
            data->active = i == 0 ? Stage::RootMaterialization : Stage::ChildMaterialization;
            auto worktree = retain(data->root->descriptor.get(), worktrees[i], true, data->active);
            // Remove the private transport locator; it is never revision authority.
            data->run(worktree.descriptor.get(), {"remote", "set-url", "origin", nodes[i].locator});
            Logger::raw_cmd("git checkout --detach " + nodes[i].commit.value());
            data->run(worktree.descriptor.get(), {"checkout", "--quiet", "--detach", nodes[i].commit.value(), "--"});
            if(i != 0) {
                const auto& edge = edges[*nodes[i].parent_edge];
                const int parent_fd = data->bindings[edge.parent].worktree.descriptor.get();
                data->run(parent_fd, {"submodule", "init", "--", edge.path});
                // Native Git creates both the relative gitfile and reverse
                // core.worktree mapping; no submodule update/fetch is needed.
                data->run(parent_fd, {"submodule", "absorbgitdirs", "--", edge.path});
            }
            data->bind(i, worktrees[i], gitdirs[i]);
        }
        data->project_root_tags();
        data->seal();
        data->prove();
        data->ready = true;
        return SourceReadyPinnedSubmoduleWorkspace(std::move(data));
    } catch(Failure& error) {
        failure = std::move(error.detail);
    } catch(const std::bad_alloc&) {
        failure = {data ? data->active : Stage::Input, Reason::ResourceLimitExceeded};
    } catch(const std::exception&) {
        failure = {data ? data->active : Stage::Input, Reason::MaterializationFailed};
    }
    if(data) {
        failure.cleanup = cleanup(*data);
        if(failure.cleanup.workspace || failure.cleanup.closure.selection) failure.abandoned_workspace = data->root_path;
    } else if(accepted.valid())
        failure.cleanup.closure = accepted.cleanup();
    return failure;
}
SourceReadyPinnedSubmoduleWorkspace::SourceReadyPinnedSubmoduleWorkspace(std::unique_ptr<PinnedSubmoduleWorkspaceData> data) noexcept : data_(std::move(data)) {
}
SourceReadyPinnedSubmoduleWorkspace::SourceReadyPinnedSubmoduleWorkspace(SourceReadyPinnedSubmoduleWorkspace&&) noexcept = default;
SourceReadyPinnedSubmoduleWorkspace::~SourceReadyPinnedSubmoduleWorkspace() noexcept {
    if(data_) static_cast<void>(cleanup());
}
bool SourceReadyPinnedSubmoduleWorkspace::valid() const noexcept {
    return data_ && data_->ready && !data_->closed && data_->accepted.valid();
}
const fs::path& SourceReadyPinnedSubmoduleWorkspace::root() const {
    if(!valid()) throw std::logic_error("Pinned workspace is inactive");
    return data_->root_path;
}
const AcceptedPinnedSubmoduleClosure& SourceReadyPinnedSubmoduleWorkspace::accepted() const {
    if(!valid()) throw std::logic_error("Pinned workspace is inactive");
    return data_->accepted;
}
PinnedWorkspaceCleanupResult SourceReadyPinnedSubmoduleWorkspace::cleanup() noexcept {
    return data_ ? PinnedSubmoduleWorkspaceAuthority::cleanup(*data_) : PinnedWorkspaceCleanupResult{};
}
std::optional<PinnedWorkspaceFailure> SourceReadyPinnedSubmoduleWorkspace::reprove() {
    if(!valid()) return PinnedWorkspaceFailure{Stage::Input, Reason::InvalidAcceptedClosure};
    PinnedWorkspaceFailure failure{Stage::SourceReadyReproof, Reason::MaterializationFailed};
    try {
        data_->deadline = Clock::now() + PHASE_TIMEOUT;
        data_->processes = 0;
        const auto result = PinnedSubmoduleWorkspaceAuthority::context(data_->accepted.closure().selection()).revalidate();
        if(const auto* error = std::get_if<InvocationOwnedSourceBuildContextFailure>(&result)) {
            failure.context = *error;
            throw Failure(std::move(failure));
        }
        data_->prove();
        return std::nullopt;
    } catch(Failure& error) {
        failure = std::move(error.detail);
    } catch(const std::bad_alloc&) {
        failure.reason = Reason::ResourceLimitExceeded;
    } catch(const std::exception&) {
    }
    failure.cleanup = cleanup();
    if(failure.cleanup.workspace || failure.cleanup.closure.selection) failure.abandoned_workspace = data_->root_path;
    return failure;
}
PinnedSubmoduleWorkspaceResult materialize_pinned_submodule_workspace(AcceptedPinnedSubmoduleClosure accepted) {
    return PinnedSubmoduleWorkspaceAuthority::materialize(std::move(accepted));
}
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
void set_pinned_workspace_test_hooks(PinnedWorkspaceTestHooks hooks) {
    g_hooks = std::move(hooks);
}
#endif
