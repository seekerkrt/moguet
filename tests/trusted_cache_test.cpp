#include "trusted_cache.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trusted_cache_test_metadata {

bool inject_staged_owner_mismatch = false;
std::string staged_leaf;
std::size_t owner_mismatch_observations = 0;

enum class HandleFault {
    None,
    ProbeSuccess,
    ZeroLength,
    ExcessiveLength,
    ShortLength,
    LongLength,
    ZeroType,
    NegativeType,
    InvalidType,
    ChangedMount,
    ChangedBytes,
};

HandleFault handle_fault = HandleFault::None;
int fid_error = 0;
int standard_error = 0;
bool emulate_standard_handle = false;
std::size_t fid_queries = 0;
std::size_t standard_queries = 0;
std::optional<struct stat> replacement_status;
struct stat expected_status{};
std::size_t stat_override_observations = 0;
std::size_t replacement_handle_queries = 0;
std::string failing_unlink_leaf;
std::string observed_unlink_leaf;
bool final_unlink_had_pin = false;
int lease_start_descriptor = -1;
int lease_ready_descriptor = -1;
bool lease_handshake_succeeded = false;

void override_replacement_identity(struct stat* status) noexcept {
    if(replacement_status.has_value() &&
       status->st_dev == replacement_status->st_dev &&
       status->st_ino == replacement_status->st_ino) {
        status->st_dev = expected_status.st_dev;
        status->st_ino = expected_status.st_ino;
        ++stat_override_observations;
    }
}

} // namespace trusted_cache_test_metadata

extern "C" int fstatat(
    int directory_descriptor, const char* path, struct stat* status,
    int flags) noexcept {
    const int result = static_cast<int>(syscall(
        SYS_newfstatat, directory_descriptor, path, status, flags));
    if(result == 0 && path != nullptr &&
       trusted_cache_test_metadata::inject_staged_owner_mismatch &&
       std::strcmp(
           path,
           trusted_cache_test_metadata::staged_leaf.c_str()) == 0) {
        status->st_uid = static_cast<uid_t>(status->st_uid ^ 1U);
        ++trusted_cache_test_metadata::owner_mismatch_observations;
    }
    if(result == 0) trusted_cache_test_metadata::override_replacement_identity(status);
    return result;
}

extern "C" int fstat(int descriptor, struct stat* status) noexcept {
    const int result = static_cast<int>(syscall(SYS_fstat, descriptor, status));
    if(result == 0) trusted_cache_test_metadata::override_replacement_identity(status);
    return result;
}

extern "C" int name_to_handle_at(
    int descriptor, const char* path, struct file_handle* handle,
    int* mount_id, int flags) noexcept {
    using namespace trusted_cache_test_metadata;
    if(replacement_status.has_value()) {
        struct stat native_status{};
        if(syscall(SYS_fstat, descriptor, &native_status) == 0 &&
           native_status.st_dev == replacement_status->st_dev &&
           native_status.st_ino == replacement_status->st_ino) {
            ++replacement_handle_queries;
        }
    }
    const bool fid_query = (flags & AT_HANDLE_FID) != 0;
    if(fid_query)
        ++fid_queries;
    else
        ++standard_queries;
    const int query_error = fid_query ? fid_error : standard_error;
    if(query_error != 0) {
        errno = query_error;
        return -1;
    }
    const unsigned int requested = handle->handle_bytes;
    if(requested == 0) {
        if(handle_fault == HandleFault::ProbeSuccess) return 0;
        if(handle_fault == HandleFault::ZeroLength ||
           handle_fault == HandleFault::ExcessiveLength) {
            handle->handle_bytes =
                handle_fault == HandleFault::ZeroLength ? 0 : MAX_HANDLE_SZ + 1;
            errno = EOVERFLOW;
            return -1;
        }
    }
    // Simulate an older standard query with real inode handles, including on
    // test filesystems which expose identity-only handles. Production receives
    // and must retain the requested standard mode; the bytes remain native.
    const int native_flags = emulate_standard_handle ? flags | AT_HANDLE_FID : flags;
    const int result = static_cast<int>(syscall(
        SYS_name_to_handle_at, descriptor, path, handle, mount_id, native_flags));
    if(result != 0) return result;
    switch(handle_fault) {
        case HandleFault::ShortLength: handle->handle_bytes = requested - 1; break;
        case HandleFault::LongLength: handle->handle_bytes = requested + 1; break;
        case HandleFault::ZeroType: handle->handle_type = 0; break;
        case HandleFault::NegativeType: handle->handle_type = -1; break;
        case HandleFault::InvalidType: handle->handle_type = 255; break;
        case HandleFault::ChangedMount: ++*mount_id; break;
        case HandleFault::ChangedBytes:
            if(handle->handle_bytes != 0) handle->f_handle[0] ^= 1;
            break;
        default: break;
    }
    return result;
}

extern "C" int unlinkat(int descriptor, const char* path, int flags) noexcept {
    using namespace trusted_cache_test_metadata;
    if(failing_unlink_leaf == path) {
        errno = EIO;
        return -1;
    }
    if(observed_unlink_leaf == path) {
        struct stat named{};
        if(syscall(SYS_newfstatat, descriptor, path, &named, AT_SYMLINK_NOFOLLOW) == 0) {
            // A fixed small range is sufficient for this isolated small-tree
            // test, and avoids opening /proc/self/fd at the final syscall.
            for(int candidate = 0; candidate < 256; ++candidate) {
                struct stat opened{};
                if(syscall(SYS_fstat, candidate, &opened) == 0 &&
                   opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) {
                    final_unlink_had_pin = true;
                }
            }
        }
    }
    const int result = static_cast<int>(syscall(SYS_unlinkat, descriptor, path, flags));
    if(result == 0 && std::strcmp(path, "alpha") == 0 &&
       lease_start_descriptor >= 0) {
        char token = 'x';
        lease_handshake_succeeded =
            write(lease_start_descriptor, &token, 1) == 1 &&
            read(lease_ready_descriptor, &token, 1) == 1 && token == 'L';
    }
    return result;
}

namespace {

namespace fs = std::filesystem;

static_assert(std::is_copy_constructible_v<ValidatedCacheRoot>);
static_assert(std::is_nothrow_destructible_v<DirCleanupGuard>);
static_assert(!std::is_copy_constructible_v<PreparedCacheCleanup>);
static_assert(!std::is_copy_assignable_v<PreparedCacheCleanup>);
static_assert(std::is_nothrow_move_constructible_v<PreparedCacheCleanup>);

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

class MetadataFaultScope final {
public:
    MetadataFaultScope() {
        trusted_cache_test_metadata::fid_queries = 0;
        trusted_cache_test_metadata::standard_queries = 0;
        trusted_cache_test_metadata::stat_override_observations = 0;
        trusted_cache_test_metadata::replacement_handle_queries = 0;
    }
    MetadataFaultScope(const MetadataFaultScope&) = delete;
    MetadataFaultScope& operator=(const MetadataFaultScope&) = delete;
    ~MetadataFaultScope() noexcept {
        using namespace trusted_cache_test_metadata;
        handle_fault = HandleFault::None;
        fid_error = 0;
        standard_error = 0;
        emulate_standard_handle = false;
        replacement_status.reset();
        failing_unlink_leaf.clear();
        observed_unlink_leaf.clear();
        final_unlink_had_pin = false;
        lease_start_descriptor = -1;
        lease_ready_descriptor = -1;
        lease_handshake_succeeded = false;
    }
};

class OccupiedDescriptors final {
    std::vector<int> descriptors_;

public:
    OccupiedDescriptors() {
        // Reserve before opening so allocation failure cannot orphan a raw FD.
        struct rlimit limit{};
        expect(getrlimit(RLIMIT_NOFILE, &limit) == 0, "Failed to read FD limit");
        descriptors_.reserve(static_cast<std::size_t>(limit.rlim_cur));
        while(true) {
            const int descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if(descriptor < 0) {
                if(errno != EMFILE) {
                    for(int occupied : descriptors_)
                        static_cast<void>(close(occupied));
                    throw std::runtime_error("FD occupancy did not reach EMFILE");
                }
                break;
            }
            descriptors_.push_back(descriptor);
        }
    }
    OccupiedDescriptors(const OccupiedDescriptors&) = delete;
    OccupiedDescriptors& operator=(const OccupiedDescriptors&) = delete;
    ~OccupiedDescriptors() noexcept {
        for(int descriptor : descriptors_)
            static_cast<void>(close(descriptor));
    }
};

template <typename Callable>
void run_in_isolated_child(Callable&& callable, const std::string& context) {
    struct rlimit before{};
    expect(getrlimit(RLIMIT_NOFILE, &before) == 0, "Failed to inspect parent limit");
    std::cout.flush();
    const pid_t child = fork();
    expect(child >= 0, "Failed to fork " + context);
    if(child == 0) {
        try {
            std::forward<Callable>(callable)();
            _exit(0);
        } catch(const std::exception& error) {
            std::cerr << context << ": " << error.what() << std::endl;
            _exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while(waited < 0 && errno == EINTR);
    expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "Isolated test failed: " + context);
    struct rlimit after{};
    expect(getrlimit(RLIMIT_NOFILE, &after) == 0 &&
               before.rlim_cur == after.rlim_cur && before.rlim_max == after.rlim_max,
           "Child test changed the parent's RLIMIT");
}

class TestPipe final {
    int read_ = -1;
    int write_ = -1;

public:
    TestPipe() {
        int descriptors[2];
        expect(pipe2(descriptors, O_CLOEXEC) == 0, "Failed to create test pipe");
        read_ = descriptors[0];
        write_ = descriptors[1];
    }
    TestPipe(const TestPipe&) = delete;
    TestPipe& operator=(const TestPipe&) = delete;
    ~TestPipe() noexcept {
        close_read();
        close_write();
    }
    int read_descriptor() const noexcept {
        return read_;
    }
    int write_descriptor() const noexcept {
        return write_;
    }
    void close_read() noexcept {
        if(read_ >= 0) static_cast<void>(close(std::exchange(read_, -1)));
    }
    void close_write() noexcept {
        if(write_ >= 0) static_cast<void>(close(std::exchange(write_, -1)));
    }
};

class DeferredWriterLease final {
    TestPipe start_;
    TestPipe ready_;
    TestPipe release_;
    pid_t child_ = -1;

public:
    explicit DeferredWriterLease(const fs::path& target) {
        child_ = fork();
        expect(child_ >= 0, "Failed to fork cooperative writer");
        if(child_ == 0) {
            start_.close_write();
            ready_.close_read();
            release_.close_write();
            char token = 0;
            if(read(start_.read_descriptor(), &token, 1) != 1) _exit(0);
            const int descriptor = open(
                target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const bool locked = descriptor >= 0 && flock(descriptor, LOCK_EX | LOCK_NB) == 0;
            token = locked ? 'L' : 'F';
            if(write(ready_.write_descriptor(), &token, 1) != 1) _exit(2);
            if(locked) static_cast<void>(read(release_.read_descriptor(), &token, 1));
            if(descriptor >= 0) static_cast<void>(close(descriptor));
            _exit(locked ? 0 : 3);
        }
        start_.close_read();
        ready_.close_write();
        release_.close_read();
    }
    DeferredWriterLease(const DeferredWriterLease&) = delete;
    DeferredWriterLease& operator=(const DeferredWriterLease&) = delete;
    ~DeferredWriterLease() noexcept {
        start_.close_write();
        release_.close_write();
        if(child_ >= 0) {
            int status = 0;
            while(waitpid(child_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    int start_descriptor() const noexcept {
        return start_.write_descriptor();
    }
    int ready_descriptor() const noexcept {
        return ready_.read_descriptor();
    }
};

class TemporaryTree final {
    fs::path path_;

public:
    TemporaryTree() {
        std::string pattern =
            (fs::temp_directory_path() / "moguet-trusted-cache-test-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::runtime_error("Failed to create trusted-cache test tree.");
        }
        path_ = created;
        fs::permissions(
            path_, fs::perms::owner_all,
            fs::perm_options::replace);
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    ~TemporaryTree() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }
};

class RemovalHookScope final {
public:
    explicit RemovalHookScope(TrustedCacheRemovalTestHook hook) {
        set_trusted_cache_removal_test_hook(std::move(hook));
    }

    RemovalHookScope(const RemovalHookScope&) = delete;
    RemovalHookScope& operator=(const RemovalHookScope&) = delete;

    ~RemovalHookScope() noexcept {
        set_trusted_cache_removal_test_hook({});
    }
};

class StagedDirectoryHookScope final {
public:
    explicit StagedDirectoryHookScope(
        TrustedCacheStagedDirectoryTestHook hook) {
        set_trusted_cache_staged_directory_test_hook(std::move(hook));
    }

    StagedDirectoryHookScope(const StagedDirectoryHookScope&) = delete;
    StagedDirectoryHookScope& operator=(
        const StagedDirectoryHookScope&) = delete;

    ~StagedDirectoryHookScope() noexcept {
        set_trusted_cache_staged_directory_test_hook({});
    }
};

class StagedOwnerMetadataOverrideScope final {
public:
    StagedOwnerMetadataOverrideScope() = default;
    StagedOwnerMetadataOverrideScope(
        const StagedOwnerMetadataOverrideScope&) = delete;
    StagedOwnerMetadataOverrideScope& operator=(
        const StagedOwnerMetadataOverrideScope&) = delete;

    ~StagedOwnerMetadataOverrideScope() noexcept {
        trusted_cache_test_metadata::inject_staged_owner_mismatch = false;
        trusted_cache_test_metadata::staged_leaf.clear();
    }

    void enable_for(const fs::path& path) {
        trusted_cache_test_metadata::staged_leaf =
            path.filename().string();
        trusted_cache_test_metadata::owner_mismatch_observations = 0;
        trusted_cache_test_metadata::inject_staged_owner_mismatch = true;
    }

    std::size_t observations() const noexcept {
        return trusted_cache_test_metadata::owner_mismatch_observations;
    }
};

class StandardOutputCapture final {
    std::ostringstream output_;
    std::streambuf* original_ = nullptr;

public:
    StandardOutputCapture()
        : original_(std::cout.rdbuf(output_.rdbuf())) {
    }

    StandardOutputCapture(const StandardOutputCapture&) = delete;
    StandardOutputCapture& operator=(const StandardOutputCapture&) = delete;

    ~StandardOutputCapture() noexcept {
        if(original_ != nullptr) std::cout.rdbuf(original_);
    }

    std::string finish() {
        if(original_ != nullptr) {
            std::cout.rdbuf(original_);
            original_ = nullptr;
        }
        return output_.str();
    }
};

class FileDescriptorLimitScope final {
    struct rlimit original_{};
    bool changed_ = false;

public:
    explicit FileDescriptorLimitScope(rlim_t soft_limit) {
        if(getrlimit(RLIMIT_NOFILE, &original_) != 0) {
            throw std::runtime_error("Failed to read the file descriptor limit.");
        }
        if(original_.rlim_cur < soft_limit) {
            throw std::runtime_error(
                "File descriptor limit is too low for the focused test.");
        }
        struct rlimit lowered = original_;
        lowered.rlim_cur = soft_limit;
        if(setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
            throw std::runtime_error(
                "Failed to lower the file descriptor limit.");
        }
        changed_ = true;
    }

    FileDescriptorLimitScope(const FileDescriptorLimitScope&) = delete;
    FileDescriptorLimitScope& operator=(
        const FileDescriptorLimitScope&) = delete;

    ~FileDescriptorLimitScope() noexcept {
        if(changed_) static_cast<void>(setrlimit(RLIMIT_NOFILE, &original_));
    }
};

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) throw std::runtime_error("Failed to create test file.");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) throw std::runtime_error("Failed to finish test file.");
}

xdg_paths::CachePaths cache_paths_for(const fs::path& cache_home) {
    xdg_paths::EnvironmentSnapshot environment;
    environment.xdg_cache_home = cache_home.string();
    environment.home = (cache_home.parent_path() / "unused-home").string();
    return xdg_paths::resolve_cache(environment);
}

ValidatedCacheRoot prepare_root(const fs::path& cache_home) {
    xdg_paths::CachePaths paths = cache_paths_for(cache_home);
    xdg_directory_safety::PreparedDirectory directory =
        xdg_directory_safety::prepare_directory(paths);
    return adopt_trusted_cache_root(paths, std::move(directory));
}

template <typename Callable>
TrustedCacheFailure expect_cache_error(
    Callable&& callable, TrustedCacheErrorCode expected_code,
    const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const TrustedCacheError& error) {
        expect(
            error.failure().code == expected_code,
            context + ": unexpected typed error code");
        return error.failure();
    }
    throw std::runtime_error(context + ": expected TrustedCacheError");
}

mode_t path_mode(const fs::path& path) {
    struct stat status{};
    if(lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("Failed to stat test path.");
    }
    return status.st_mode & 07777;
}

struct PathIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
};

PathIdentity path_identity(const fs::path& path) {
    struct stat status{};
    if(lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("Failed to inspect test path identity.");
    }
    return PathIdentity{
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino)};
}

bool process_holds_identity(
    const PathIdentity& expected, std::optional<nlink_t> link_count =
                                      std::nullopt) {
    for(const fs::directory_entry& entry :
        fs::directory_iterator("/proc/self/fd")) {
        int descriptor = -1;
        try {
            descriptor = std::stoi(entry.path().filename().string());
        } catch(const std::exception&) {
            continue;
        }
        struct stat status{};
        if(fstat(descriptor, &status) != 0) continue;
        if(static_cast<std::uintmax_t>(status.st_dev) == expected.device &&
           static_cast<std::uintmax_t>(status.st_ino) == expected.inode &&
           (!link_count.has_value() ||
            status.st_nlink == link_count.value())) {
            return true;
        }
    }
    return false;
}

std::size_t open_descriptor_count() {
    return static_cast<std::size_t>(std::distance(
        fs::directory_iterator("/proc/self/fd"),
        fs::directory_iterator()));
}

struct TreeEntrySnapshot {
    fs::path relative_path;
    mode_t type_and_mode = 0;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::string symlink_target;
    std::string contents;

    bool operator==(const TreeEntrySnapshot&) const = default;
};

TreeEntrySnapshot snapshot_entry(
    const fs::path& root, const fs::path& path) {
    struct stat status{};
    if(lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("Failed to snapshot legacy entry.");
    }
    TreeEntrySnapshot snapshot{
        path.lexically_relative(root.parent_path()),
        status.st_mode,
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_uid),
        {},
        {}};
    if(S_ISLNK(status.st_mode)) {
        snapshot.symlink_target = fs::read_symlink(path).string();
    } else if(S_ISREG(status.st_mode)) {
        std::ifstream input(path, std::ios::binary);
        snapshot.contents.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }
    return snapshot;
}

std::vector<TreeEntrySnapshot> snapshot_tree(const fs::path& root) {
    std::vector<TreeEntrySnapshot> snapshot;
    snapshot.push_back(snapshot_entry(root, root));
    for(const fs::directory_entry& entry :
        fs::recursive_directory_iterator(root)) {
        snapshot.push_back(snapshot_entry(root, entry.path()));
    }
    std::sort(
        snapshot.begin(), snapshot.end(),
        [](const TreeEntrySnapshot& lhs, const TreeEntrySnapshot& rhs) {
            return lhs.relative_path < rhs.relative_path;
        });
    return snapshot;
}

void test_bridge_layout_and_cwd_independence() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::path unrelated_cwd = tree.path() / "unrelated-cwd";
    fs::create_directory(cache_home);
    fs::create_directory(unrelated_cwd);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);

    ValidatedCacheRoot root = prepare_root(cache_home);
    expect(root.path() == cache_home / "moguet", "Cache layout is not Moguet.");
    expect(path_mode(root.path()) == 0700, "New cache root mode is not 0700.");

    fs::path original_cwd = fs::current_path();
    fs::current_path(unrelated_cwd);
    try {
        ValidatedCachePath child =
            create_trusted_cache_directory(root, "checkout");
        expect(
            child.path() == cache_home / "moguet" / "checkout",
            "Relative child used current working directory.");
        expect(!fs::exists(unrelated_cwd / "checkout"),
               "Cache child escaped into current working directory.");
        for(const fs::directory_entry& entry :
            fs::directory_iterator(root.path())) {
            expect(
                !entry.path().filename().string().starts_with(
                    ".moguet-cache-create-"),
                "Successful child creation retained its staging leaf.");
        }
    } catch(...) {
        fs::current_path(original_cwd);
        throw;
    }
    fs::current_path(original_cwd);

    expect_cache_error(
        [&]() {
            static_cast<void>(require_trusted_cache_path(
                root, "../escape",
                CachePathRequirement::ExistingOrMissing));
        },
        TrustedCacheErrorCode::ChildEscape,
        "direct child escape");
}

void test_prepared_directory_adoption_is_single_consume() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);

    xdg_paths::CachePaths paths = cache_paths_for(cache_home);
    xdg_directory_safety::PreparedDirectory directory =
        xdg_directory_safety::prepare_directory(paths);
    ValidatedCacheRoot root = adopt_trusted_cache_root(
        paths, std::move(directory));
    root.require_unchanged_identity();

    bool source_is_invalid = false;
    try {
        directory.require_unchanged_identity();
    } catch(const xdg_directory_safety::PreparationError&) {
        source_is_invalid = true;
    }
    expect(source_is_invalid,
           "PreparedDirectory source remained valid after cache adoption.");

    expect_cache_error(
        [&]() {
            static_cast<void>(adopt_trusted_cache_root(
                paths, std::move(directory)));
        },
        TrustedCacheErrorCode::InvalidBoundary,
        "second PreparedDirectory adoption");
    root.require_unchanged_identity();
}

void test_root_and_child_replacement_refusal() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);

    {
        ValidatedCacheRoot root = prepare_root(cache_home);
        fs::path moved_root = tree.path() / "moved-original-root";
        fs::rename(root.path(), moved_root);
        fs::create_directory(root.path());
        fs::permissions(root.path(), fs::perms::owner_all,
                        fs::perm_options::replace);
        expect_cache_error(
            [&]() { root.require_unchanged_identity(); },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "root replacement");
        expect(fs::is_directory(moved_root), "Original root was removed.");
        expect(fs::is_directory(root.path()), "Replacement root was removed.");
    }

    fs::remove_all(cache_home / "moguet");
    ValidatedCacheRoot root = prepare_root(cache_home);
    ValidatedCachePath original =
        create_trusted_cache_directory(root, "checkout");
    write_file(original.path() / "sentinel", "original\n");
    fs::path moved_child = tree.path() / "moved-original-checkout";
    fs::rename(original.path(), moved_child);
    fs::create_directory(original.path());
    write_file(original.path() / "replacement", "replacement\n");

    expect_cache_error(
        [&]() { remove_trusted_cache_path(original); },
        TrustedCacheErrorCode::ConcurrentReplacement,
        "child replacement removal");
    expect(fs::exists(moved_child / "sentinel"),
           "Moved original child was followed and deleted.");
    expect(fs::exists(original.path() / "replacement"),
           "Replacement child was deleted.");
}

void test_symlink_and_cleanup_preflight() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);

    ValidatedCachePath safe = create_trusted_cache_directory(root, "safe");
    fs::path outside = tree.path() / "outside";
    fs::create_directory(outside);
    write_file(outside / "sentinel", "outside\n");
    fs::create_directory_symlink(outside, root.path() / "unsafe");

    TrustedCacheFailure failure = expect_cache_error(
        [&]() { static_cast<void>(preflight_cache_cleanup(root)); },
        TrustedCacheErrorCode::Symlink,
        "cleanup symlink preflight");
    expect(
        failure.stage == TrustedCacheStage::CleanupPreflight,
        "Cleanup preflight failure has the wrong stage.");
    expect(fs::is_directory(safe.path()),
           "Cleanup preflight failure deleted a safe target.");
    expect(fs::is_symlink(root.path() / "unsafe"),
           "Cleanup preflight failure changed the symlink.");
    expect(fs::exists(outside / "sentinel"),
           "Cleanup preflight followed a symlink outside the root.");
}

void test_nested_symlink_cleanup_pins_link_without_following() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);
    ValidatedCachePath target =
        create_trusted_cache_directory(root, "checkout");
    fs::path outside = tree.path() / "outside";
    fs::create_directory(outside);
    write_file(outside / "sentinel", "outside\n");
    fs::create_directory_symlink(outside, target.path() / "outside-link");

    const std::size_t descriptors_before = open_descriptor_count();
    PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
    remove_preflighted_cache_paths(std::move(cleanup));

    expect(!fs::exists(target.path()),
           "Cleanup retained a tree containing a nested symlink.");
    expect(fs::exists(outside / "sentinel"),
           "Cleanup followed a nested symlink outside the cache.");
    expect(open_descriptor_count() == descriptors_before,
           "Successful removal leaked a pinned descriptor.");
}

void test_cleanup_capability_revalidation_is_global() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);

    create_trusted_cache_directory(root, "alpha");
    create_trusted_cache_directory(root, "beta");
    PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);

    fs::path moved_beta = tree.path() / "moved-beta";
    fs::rename(root.path() / "beta", moved_beta);
    fs::create_directory_symlink(tree.path(), root.path() / "beta");

    expect_cache_error(
        [&]() { remove_preflighted_cache_paths(std::move(cleanup)); },
        TrustedCacheErrorCode::ConcurrentReplacement,
        "global cleanup capability revalidation");
    expect(fs::is_directory(root.path() / "alpha"),
           "Cleanup deleted alpha before every target revalidated.");
    expect(fs::is_directory(moved_beta),
           "Cleanup tracked the original beta outside the cache root.");
    expect(fs::is_symlink(root.path() / "beta"),
           "Cleanup deleted the beta replacement symlink.");
}

void test_repeated_cleanup_preflight_restarts_directory_scan() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);

    create_trusted_cache_directory(root, "alpha");
    expect(preflight_cache_cleanup(root).size() == 1,
           "First cleanup preflight did not list alpha.");
    create_trusted_cache_directory(root, "beta");
    expect(preflight_cache_cleanup(root).size() == 2,
           "Repeated cleanup preflight reused an exhausted directory offset.");
}

void test_cleanup_refuses_held_package_base_lease() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(
        cache_home, fs::perms::owner_all,
        fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);
    ValidatedCachePath checkout =
        create_trusted_cache_directory(root, "leased-checkout");

    const int descriptor = open(
        checkout.path().c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    expect(descriptor >= 0, "Failed to open cleanup lease fixture");
    expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0,
           "Failed to hold cleanup lease fixture");
    TrustedCacheFailure failure = expect_cache_error(
        [&]() {
            static_cast<void>(preflight_cache_cleanup(root));
        },
        TrustedCacheErrorCode::ConcurrentReplacement,
        "cleanup PackageBase lease contention");
    expect(failure.stage == TrustedCacheStage::CleanupPreflight,
           "Cleanup lease contention has the wrong stage.");
    expect(fs::is_directory(checkout.path()),
           "Cleanup removed a checkout while its lease was held.");
    expect(flock(descriptor, LOCK_UN) == 0,
           "Failed to release cleanup lease fixture");
    expect(close(descriptor) == 0,
           "Failed to close cleanup lease fixture");

    PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
    remove_preflighted_cache_paths(std::move(cleanup));
    expect(!fs::exists(checkout.path()),
           "Cleanup did not proceed after PackageBase lease release.");
}

void test_empty_cleanup_capability_is_consumable() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);

    const std::size_t descriptors_before = open_descriptor_count();
    PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
    expect(cleanup.empty(), "Empty cache produced a non-empty cleanup plan.");
    expect(cleanup.size() == 0, "Empty cleanup plan reported target entries.");
    remove_preflighted_cache_paths(std::move(cleanup));
    expect(fs::is_empty(root.path()), "Empty cleanup changed the cache root.");
    expect(open_descriptor_count() == descriptors_before,
           "Empty cleanup capability leaked a descriptor.");
}

void test_cleanup_capability_reports_fd_exhaustion_before_removal() {
    run_in_isolated_child([] {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        for(std::size_t index = 0; index < 32; ++index) {
            write_file(
                root.path() / ("entry-" + std::to_string(index)),
                "retained\n");
        }
        const std::vector<TreeEntrySnapshot> before = snapshot_tree(root.path());
        const std::size_t descriptors_before = open_descriptor_count();
        TrustedCacheFailure failure;
        {
            FileDescriptorLimitScope lowered_limit(
                static_cast<rlim_t>(descriptors_before + 6));
            OccupiedDescriptors occupancy;
            failure = expect_cache_error(
                [&]() {
                    static_cast<void>(preflight_cache_cleanup(root));
                },
                TrustedCacheErrorCode::MetadataFailure,
                "cleanup aggregate file descriptor exhaustion");
        }

        expect(failure.stage == TrustedCacheStage::CleanupPreflight,
               "FD exhaustion was not reported during cleanup preflight.");
        expect(failure.system_error.has_value() &&
                   failure.system_error->value() == EMFILE,
               "FD exhaustion did not retain the Too many open files error.");
        expect(snapshot_tree(root.path()) == before,
               "FD exhaustion removed cache data during capability construction.");
        expect(open_descriptor_count() == descriptors_before,
               "Failed aggregate preflight leaked retained descriptors.");
    },
                          "true preflight EMFILE");
}

void test_cleanup_generation_failures_are_closed() {
    using namespace trusted_cache_test_metadata;
    const std::vector<HandleFault> malformed = {
        HandleFault::ProbeSuccess, HandleFault::ZeroLength,
        HandleFault::ExcessiveLength, HandleFault::ShortLength,
        HandleFault::LongLength, HandleFault::ZeroType,
        HandleFault::NegativeType, HandleFault::InvalidType};
    for(bool consume_phase : {false, true}) {
        for(std::size_t scenario = 0; scenario < malformed.size() + 3; ++scenario) {
            TemporaryTree tree;
            fs::path cache_home = tree.path() / "cache";
            fs::create_directory(cache_home);
            ValidatedCacheRoot root = prepare_root(cache_home);
            write_file(root.path() / "alpha", "keep\n");
            const auto before = snapshot_tree(root.path());
            const auto descriptors_before = open_descriptor_count();
            std::optional<PreparedCacheCleanup> cleanup;
            if(consume_phase) cleanup.emplace(preflight_cache_cleanup(root));
            MetadataFaultScope faults;
            int expected_errno = 0;
            if(scenario < malformed.size()) {
                handle_fault = malformed[scenario];
            } else {
                const int errors[] = {EOPNOTSUPP, ENOSYS, EPERM};
                expected_errno = errors[scenario - malformed.size()];
                fid_error = expected_errno;
                standard_error = expected_errno;
            }
            const auto failure = expect_cache_error(
                [&] {
                    if(cleanup.has_value()) {
                        remove_preflighted_cache_paths(std::move(*cleanup));
                    } else {
                        static_cast<void>(preflight_cache_cleanup(root));
                    }
                },
                TrustedCacheErrorCode::MetadataFailure, "generation evidence refusal");
            expect(failure.stage == TrustedCacheStage::CleanupPreflight,
                   "Generation failure escaped the global preflight stage");
            expect(expected_errno == 0
                       ? !failure.system_error.has_value()
                       : failure.system_error.has_value() &&
                             failure.system_error->value() == expected_errno,
                   "Generation failure invented or lost errno");
            expect(standard_queries == 0,
                   "Malformed/unsupported evidence used a standard fallback");
            expect(snapshot_tree(root.path()) == before,
                   "Invalid generation evidence deleted a target");
            expect(open_descriptor_count() == descriptors_before,
                   "Generation failure leaked a descriptor");
        }
    }
    for(HandleFault fault : {HandleFault::ChangedMount, HandleFault::ChangedBytes}) {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "cache";
        fs::create_directory(cache_home);
        ValidatedCacheRoot root = prepare_root(cache_home);
        write_file(root.path() / "entry", "keep\n");
        const auto descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
        MetadataFaultScope faults;
        handle_fault = fault;
        expect_cache_error(
            [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
            TrustedCacheErrorCode::ConcurrentReplacement, "generation mismatch");
        expect(fs::exists(root.path() / "entry"), "Generation drift deleted a target");
        expect(open_descriptor_count() == descriptors_before,
               "Generation drift leaked descriptors");
    }
}

void test_cleanup_generation_query_mode_is_fixed() {
    using namespace trusted_cache_test_metadata;
    for(bool standard_mode : {false, true}) {
        for(bool fail_consume : {false, true}) {
            TemporaryTree tree;
            fs::path cache_home = tree.path() / "cache";
            fs::create_directory(cache_home);
            ValidatedCacheRoot root = prepare_root(cache_home);
            auto target = create_trusted_cache_directory(root, "checkout");
            write_file(target.path() / "file", "entry\n");
            const auto descriptors_before = open_descriptor_count();
            MetadataFaultScope faults;
            if(standard_mode) {
                fid_error = EINVAL;
                emulate_standard_handle = true;
            }
            PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
            const auto initial_fid_queries = fid_queries;
            const auto initial_standard_queries = standard_queries;
            if(standard_mode) {
                expect(initial_fid_queries == 2 && initial_standard_queries > 2,
                       "Initial FID feature negotiation did not occur per node");
                fid_error = 0; // FID is now available, but must not be selected again.
                if(fail_consume) standard_error = EOPNOTSUPP;
            } else {
                expect(initial_standard_queries == 0, "Native mode used standard query");
                if(fail_consume) {
                    fid_error = EINVAL; // Standard would succeed, but mode is sealed.
                    emulate_standard_handle = true;
                }
            }
            if(fail_consume) {
                const auto failure = expect_cache_error(
                    [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
                    TrustedCacheErrorCode::MetadataFailure, "fixed generation query mode");
                expect(failure.system_error.has_value() &&
                           failure.system_error->value() ==
                               (standard_mode ? EOPNOTSUPP : EINVAL),
                       "Fixed-mode refusal lost API error");
                expect(fs::exists(target.path() / "file"), "Mode failure deleted a file");
            } else {
                remove_preflighted_cache_paths(std::move(cleanup));
                expect(fs::is_empty(root.path()), "Fixed-mode cleanup did not finish");
            }
            expect(standard_mode ? fid_queries == initial_fid_queries
                                 : standard_queries == initial_standard_queries,
                   "Revalidation switched generation query mode");
            expect(open_descriptor_count() == descriptors_before,
                   "Fixed-mode cleanup leaked descriptors");
        }
    }
}

void test_cleanup_refuses_same_stat_different_generation() {
    using namespace trusted_cache_test_metadata;
    for(bool directory_target : {false, true}) {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "cache";
        fs::create_directory(cache_home);
        ValidatedCacheRoot root = prepare_root(cache_home);
        write_file(root.path() / "alpha", "unrelated\n");
        const fs::path target = root.path() / "beta";
        if(directory_target)
            create_trusted_cache_directory(root, "beta");
        else
            write_file(target, "original\n");
        struct stat original{};
        expect(lstat(target.c_str(), &original) == 0, "Failed to inspect original");
        const auto descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
        const fs::path replacement = tree.path() / "replacement";
        if(directory_target)
            fs::create_directory(replacement);
        else
            write_file(replacement, "replacement\n");
        fs::permissions(replacement, static_cast<fs::perms>(original.st_mode & 07777),
                        fs::perm_options::replace);
        // Create the replacement while the original still exists: distinct
        // native inode generations are deterministic, with no allocator loop.
        fs::rename(replacement, target);
        struct stat actual{};
        expect(lstat(target.c_str(), &actual) == 0, "Failed to inspect replacement");
        expect(actual.st_dev == original.st_dev && actual.st_ino != original.st_ino &&
                   actual.st_mode == original.st_mode && actual.st_uid == original.st_uid,
               "ABA fixture does not have equal security metadata and a new inode");
        MetadataFaultScope faults;
        replacement_status = actual;
        expected_status = original;
        const auto failure = expect_cache_error(
            [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
            TrustedCacheErrorCode::ConcurrentReplacement, "same-stat new-generation ABA");
        expect(failure.stage == TrustedCacheStage::CleanupPreflight,
               "ABA bypassed the global barrier");
        expect(stat_override_observations >= 1 && replacement_handle_queries > 0,
               "ABA fixture did not exercise both matching stat and real generation");
        expect(fs::exists(root.path() / "alpha") && fs::exists(target),
               "ABA removed a replacement or an earlier target");
        expect(open_descriptor_count() == descriptors_before, "ABA leaked descriptors");
    }
}

void test_cleanup_inventory_changes_preserve_global_barrier() {
    for(bool add_child : {false, true}) {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "cache";
        fs::create_directory(cache_home);
        ValidatedCacheRoot root = prepare_root(cache_home);
        write_file(root.path() / "alpha", "unrelated\n");
        auto beta = create_trusted_cache_directory(root, "beta");
        write_file(beta.path() / "existing", "existing\n");
        const auto descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
        if(add_child)
            write_file(beta.path() / "added", "added\n");
        else
            fs::remove(beta.path() / "existing");
        const auto before = snapshot_tree(root.path());
        const auto failure = expect_cache_error(
            [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
            TrustedCacheErrorCode::ConcurrentReplacement, "directory inventory change");
        expect(failure.stage == TrustedCacheStage::CleanupPreflight,
               "Inventory failure passed the global barrier");
        expect(snapshot_tree(root.path()) == before,
               "Inventory failure deleted an earlier target");
        expect(open_descriptor_count() == descriptors_before,
               "Inventory failure leaked descriptors");
    }
}

void test_cleanup_releases_preflight_leases_and_rechecks_busy_targets() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache";
    fs::create_directory(cache_home);
    ValidatedCacheRoot root = prepare_root(cache_home);
    create_trusted_cache_directory(root, "alpha");
    auto beta = create_trusted_cache_directory(root, "beta");
    const auto descriptors_before = open_descriptor_count();
    {
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
        DeferredWriterLease writer(beta.path());
        char token = 'x';
        expect(write(writer.start_descriptor(), &token, 1) == 1 &&
                   read(writer.ready_descriptor(), &token, 1) == 1 && token == 'L',
               "Prepared capability retained beta's cooperative lease");
        const auto failure = expect_cache_error(
            [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
            TrustedCacheErrorCode::ConcurrentReplacement, "busy target at global barrier");
        expect(failure.stage == TrustedCacheStage::CleanupPreflight,
               "Global lease refusal has the wrong stage");
        expect(fs::is_directory(root.path() / "alpha") && fs::is_directory(beta.path()),
               "Global lease refusal deleted an earlier target");
    }
    expect(open_descriptor_count() == descriptors_before,
           "Global lease refusal leaked descriptors");
}

void test_cleanup_late_writer_lease_is_partial_failure() {
    using namespace trusted_cache_test_metadata;
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache";
    fs::create_directory(cache_home);
    ValidatedCacheRoot root = prepare_root(cache_home);
    create_trusted_cache_directory(root, "alpha");
    auto beta = create_trusted_cache_directory(root, "beta");
    write_file(beta.path() / "sentinel", "busy\n");
    write_file(root.path() / "gamma", "unattempted\n");
    const auto descriptors_before = open_descriptor_count();
    {
        DeferredWriterLease writer(beta.path());
        MetadataFaultScope faults;
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
        lease_start_descriptor = writer.start_descriptor();
        lease_ready_descriptor = writer.ready_descriptor();
        // unlinkat interposition starts the independent writer only after
        // alpha's successful unlink, before beta's mutation lease acquisition.
        const auto failure = expect_cache_error(
            [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
            TrustedCacheErrorCode::ConcurrentReplacement, "late cooperative writer");
        expect(lease_handshake_succeeded, "Writer did not acquire beta between targets");
        expect(failure.stage == TrustedCacheStage::RecursiveRemoval &&
                   failure.system_error.has_value() &&
                   (failure.system_error->value() == EWOULDBLOCK ||
                    failure.system_error->value() == EAGAIN),
               "Late lease contention has wrong phase/error");
        expect(!fs::exists(root.path() / "alpha"), "Completed alpha was not deleted");
        expect(fs::exists(beta.path() / "sentinel") && fs::exists(root.path() / "gamma"),
               "Busy beta or unattempted gamma was deleted");
    }
    expect(open_descriptor_count() == descriptors_before,
           "Lease failure/unwind leaked descriptors");
    // Releasing the child-held lease permits a new, explicit cleanup attempt.
    remove_preflighted_cache_paths(preflight_cache_cleanup(root));
    expect(fs::is_empty(root.path()), "Lease was not released after writer exit");
}

void test_cleanup_mutation_failures_and_final_pin() {
    using namespace trusted_cache_test_metadata;
    for(bool generation_failure : {false, true}) {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "cache";
        fs::create_directory(cache_home);
        ValidatedCacheRoot root = prepare_root(cache_home);
        write_file(root.path() / "alpha", "completed\n");
        write_file(root.path() / "beta", "failed\n");
        write_file(root.path() / "gamma", "unattempted\n");
        const auto descriptors_before = open_descriptor_count();
        MetadataFaultScope faults;
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
        observed_unlink_leaf = "alpha";
        std::size_t roots_seen = 0;
        RemovalHookScope hook([&](std::size_t depth) {
            if(depth != 0 || ++roots_seen != 2) return;
            if(generation_failure)
                fid_error = EOPNOTSUPP;
            else
                failing_unlink_leaf = "beta";
        });
        const auto failure = expect_cache_error(
            [&] { remove_preflighted_cache_paths(std::move(cleanup)); },
            generation_failure ? TrustedCacheErrorCode::MetadataFailure
                               : TrustedCacheErrorCode::RemovalFailure,
            "mid-mutation failure");
        expect(failure.stage == TrustedCacheStage::RecursiveRemoval &&
                   failure.system_error.has_value() &&
                   failure.system_error->value() == (generation_failure ? EOPNOTSUPP : EIO),
               "Mutation failure lost stage/errno");
        expect(final_unlink_had_pin, "Regular file was not pinned through unlinkat");
        expect(!fs::exists(root.path() / "alpha") &&
                   fs::exists(root.path() / "beta") && fs::exists(root.path() / "gamma"),
               "Mutation failure did not preserve partial/unattempted outcome");
        expect(open_descriptor_count() == descriptors_before,
               "Mutation failure leaked descriptors");
    }
}

void test_cleanup_large_trees_have_bounded_descriptors() {
    for(int shape = 0; shape < 3; ++shape) {
        for(std::size_t entries : {std::size_t{512}, std::size_t{4096}}) {
            run_in_isolated_child([&] {
                TemporaryTree tree;
                fs::path cache_home = tree.path() / "cache";
                fs::create_directory(cache_home);
                ValidatedCacheRoot root = prepare_root(cache_home);
                fs::path leaf_parent = root.path();
                if(shape == 2) {
                    // Eight directory levels plus file leaves gives depth nine.
                    for(int depth = 0; depth < 8; ++depth) {
                        leaf_parent /= "nested";
                        fs::create_directory(leaf_parent);
                    }
                    write_file(root.path() / "shallow-file", "shallow\n");
                    fs::create_directory(root.path() / "shallow-directory");
                }
                for(std::size_t index = 0; index < entries; ++index) {
                    const fs::path entry = leaf_parent / ("entry-" + std::to_string(index));
                    if(shape == 1)
                        fs::create_directory(entry);
                    else
                        write_file(entry, "bounded\n");
                }
                struct rlimit original{};
                expect(getrlimit(RLIMIT_NOFILE, &original) == 0, "Failed to inspect child limit");
                const rlim_t limit = std::min<rlim_t>(256, original.rlim_cur);
                const auto descriptors_before = open_descriptor_count();
                expect(limit > descriptors_before + 24, "Insufficient child FD headroom");
                {
                    FileDescriptorLimitScope lowered(limit);
                    PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
                    const auto descriptors_retained = open_descriptor_count();
                    expect(descriptors_retained == descriptors_before,
                           "Capability retained per-entry pins or per-target leases");
                    remove_preflighted_cache_paths(std::move(cleanup));
                    expect(fs::is_empty(root.path()), "Low-RLIMIT cleanup left cache entries");
                    expect(open_descriptor_count() == descriptors_before,
                           "Successful bounded cleanup leaked descriptors");
                    struct rlimit current{};
                    expect(getrlimit(RLIMIT_NOFILE, &current) == 0 &&
                               current.rlim_cur == limit && current.rlim_max == original.rlim_max,
                           "Cleanup raised a limit or changed the hard limit");
                }
                struct rlimit restored{};
                expect(getrlimit(RLIMIT_NOFILE, &restored) == 0 &&
                           restored.rlim_cur == original.rlim_cur &&
                           restored.rlim_max == original.rlim_max,
                       "Child FD limit did not restore");
                std::cout << "bounded cleanup: shape=" << shape << " entries=" << entries
                          << " soft=" << limit << " retained_delta=0" << std::endl;
            },
                                  "bounded N/T/depth cleanup");
        }
    }
}

void test_cleanup_capability_preserves_generation_until_consumed() {
    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "top-level-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath checkout =
            create_trusted_cache_directory(root, "alpha");
        write_file(checkout.path() / "sentinel", "original\n");
        create_trusted_cache_directory(root, "unrelated");
        const PathIdentity original = path_identity(checkout.path());
        const std::size_t descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);

        expect(!process_holds_identity(original),
               "Metadata plan unexpectedly retained the top-level inode.");
        fs::path moved_original = tree.path() / "moved-alpha";
        fs::rename(checkout.path(), moved_original);
        fs::create_directory(checkout.path());
        fs::permissions(
            checkout.path(), fs::perms::owner_all,
            fs::perm_options::replace);
        write_file(checkout.path() / "replacement", "replacement\n");

        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "top-level replacement before cleanup consumption");
        expect(fs::exists(moved_original / "sentinel"),
               "Cleanup followed the top-level original outside the root.");
        expect(fs::exists(checkout.path() / "replacement"),
               "Cleanup removed the top-level replacement.");
        expect(fs::is_directory(root.path() / "unrelated"),
               "Cleanup started deleting unrelated targets before failure.");
        expect(open_descriptor_count() == descriptors_before,
               "Failed top-level cleanup leaked retained descriptors.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "nested-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath checkout =
            create_trusted_cache_directory(root, "checkout");
        fs::path nested = checkout.path() / "nested";
        fs::create_directory(nested);
        write_file(nested / "sentinel", "original\n");
        write_file(checkout.path() / "unrelated", "unrelated\n");
        const PathIdentity original = path_identity(nested);
        const std::size_t descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);

        expect(!process_holds_identity(original),
               "Metadata plan unexpectedly retained the nested inode.");
        fs::path moved_original = tree.path() / "moved-nested";
        fs::rename(nested, moved_original);
        fs::create_directory(nested);
        fs::permissions(
            nested, fs::perms::owner_all, fs::perm_options::replace);
        write_file(nested / "replacement", "replacement\n");

        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "nested replacement before cleanup consumption");
        expect(fs::exists(moved_original / "sentinel"),
               "Cleanup followed the nested original outside the root.");
        expect(fs::exists(nested / "replacement"),
               "Cleanup removed the nested replacement.");
        expect(fs::exists(checkout.path() / "unrelated"),
               "Cleanup changed an unrelated sibling before failure.");
        expect(open_descriptor_count() == descriptors_before,
               "Failed nested cleanup leaked retained descriptors.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "file-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        fs::path original_path = root.path() / "ordinary-file";
        write_file(original_path, "original\n");
        write_file(root.path() / "unrelated", "unrelated\n");
        const PathIdentity original = path_identity(original_path);
        const std::size_t descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);

        expect(!process_holds_identity(original),
               "Metadata plan unexpectedly retained the regular-file inode.");
        fs::path moved_original = tree.path() / "moved-file";
        fs::rename(original_path, moved_original);
        write_file(original_path, "replacement\n");

        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "file replacement before cleanup consumption");
        expect(snapshot_entry(tree.path(), moved_original).contents ==
                   "original\n",
               "Cleanup changed the moved regular-file original.");
        expect(snapshot_entry(root.path(), original_path).contents ==
                   "replacement\n",
               "Cleanup removed or changed the regular-file replacement.");
        expect(fs::exists(root.path() / "unrelated"),
               "Cleanup removed an unrelated file before failure.");
        expect(open_descriptor_count() == descriptors_before,
               "Failed regular-file cleanup leaked retained descriptors.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "symlink-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath checkout =
            create_trusted_cache_directory(root, "checkout");
        fs::path outside_original = tree.path() / "outside-original";
        fs::path outside_replacement = tree.path() / "outside-replacement";
        fs::create_directory(outside_original);
        fs::create_directory(outside_replacement);
        write_file(outside_original / "sentinel", "original\n");
        write_file(outside_replacement / "sentinel", "replacement\n");
        fs::path link = checkout.path() / "link";
        fs::create_directory_symlink(outside_original, link);
        write_file(checkout.path() / "unrelated", "unrelated\n");
        const PathIdentity original = path_identity(link);
        const std::size_t descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);

        expect(!process_holds_identity(original),
               "Metadata plan unexpectedly retained the symlink inode.");
        fs::path moved_link = tree.path() / "moved-link";
        fs::rename(link, moved_link);
        fs::create_directory_symlink(outside_replacement, link);

        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "symlink replacement before cleanup consumption");
        expect(fs::read_symlink(moved_link) == outside_original,
               "Cleanup changed the moved original symlink.");
        expect(fs::read_symlink(link) == outside_replacement,
               "Cleanup removed or changed the replacement symlink.");
        expect(fs::exists(outside_original / "sentinel") &&
                   fs::exists(outside_replacement / "sentinel"),
               "Cleanup followed a symlink outside the cache root.");
        expect(fs::exists(checkout.path() / "unrelated"),
               "Cleanup removed a symlink sibling before failure.");
        expect(open_descriptor_count() == descriptors_before,
               "Failed symlink cleanup leaked retained descriptors.");
    }
}

void test_removal_plan_rejects_unlinked_replacements() {
    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "top-level-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath target =
            create_trusted_cache_directory(root, "checkout");
        const PathIdentity original = path_identity(target.path());
        const std::size_t descriptors_before = open_descriptor_count();
        PreparedCacheCleanup cleanup =
            preflight_cache_cleanup(root);

        bool hook_ran = false;
        RemovalHookScope hook([&](std::size_t depth) {
            if(depth != 0 || hook_ran) return;
            hook_ran = true;
            expect(fs::remove(target.path()),
                   "Failed to unlink original top-level directory.");
            expect(process_holds_identity(original),
                   "Removal plan did not pin the original top-level inode.");
            fs::create_directory(target.path());
            write_file(target.path() / "replacement", "replacement\n");
        });
        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "pinned top-level directory replacement");
        expect(hook_ran, "Top-level replacement hook did not run.");
        expect(fs::exists(target.path() / "replacement"),
               "Replacement top-level directory was deleted.");
        expect(open_descriptor_count() == descriptors_before,
               "Failed removal leaked a pinned descriptor.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "nested-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath target =
            create_trusted_cache_directory(root, "checkout");
        fs::path nested = target.path() / "nested";
        fs::create_directory(nested);
        const PathIdentity original = path_identity(nested);
        PreparedCacheCleanup cleanup =
            preflight_cache_cleanup(root);

        bool hook_ran = false;
        RemovalHookScope hook([&](std::size_t depth) {
            if(depth != 0 || hook_ran) return;
            hook_ran = true;
            expect(fs::remove(nested),
                   "Failed to unlink original nested directory.");
            expect(!process_holds_identity(original),
                   "Removal plan retained the original nested inode.");
            fs::create_directory(nested);
            write_file(nested / "replacement", "replacement\n");
        });
        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "pinned nested directory replacement");
        expect(hook_ran, "Nested replacement hook did not run.");
        expect(fs::exists(nested / "replacement"),
               "Replacement nested directory was deleted.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "file-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        fs::path target = root.path() / "ordinary-file";
        write_file(target, "original\n");
        const PathIdentity original = path_identity(target);
        PreparedCacheCleanup cleanup =
            preflight_cache_cleanup(root);

        bool hook_ran = false;
        RemovalHookScope hook([&](std::size_t depth) {
            if(depth != 0 || hook_ran) return;
            hook_ran = true;
            expect(fs::remove(target),
                   "Failed to unlink original regular file.");
            expect(!process_holds_identity(original, 0),
                   "Removal plan retained the unlinked regular file.");
            write_file(target, "replacement\n");
        });
        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "pinned regular-file replacement");
        expect(hook_ran, "Regular-file replacement hook did not run.");
        expect(snapshot_entry(root.path(), target).contents == "replacement\n",
               "Replacement regular file was deleted or changed.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "rollback-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath target =
            create_trusted_cache_directory(root, "checkout");
        const PathIdentity original = path_identity(target.path());

        bool hook_ran = false;
        RemovalHookScope hook([&](std::size_t depth) {
            if(depth != 0 || hook_ran) return;
            hook_ran = true;
            expect(fs::remove(target.path()),
                   "Failed to unlink original rollback target.");
            expect(process_holds_identity(original),
                   "Rollback plan did not pin its original inode.");
            fs::create_directory(target.path());
            write_file(target.path() / "replacement", "replacement\n");
        });
        {
            DirCleanupGuard rollback(target);
        }
        expect(hook_ran, "Rollback replacement hook did not run.");
        expect(fs::exists(target.path() / "replacement"),
               "Rollback deleted the replacement target.");
    }
}

void test_removal_refuses_revoked_root_and_ancestor_lineage() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);

    {
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath target =
            create_trusted_cache_directory(root, "checkout");
        write_file(target.path() / "sentinel", "root lineage\n");
        PreparedCacheCleanup cleanup =
            preflight_cache_cleanup(root);
        fs::path moved_root = tree.path() / "moved-cache-root";
        bool mutated = false;
        RemovalHookScope hook([&](std::size_t depth) {
            if(depth != 0 || mutated) return;
            mutated = true;
            fs::rename(root.path(), moved_root);
        });
        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "revoked root removal");
        expect(fs::exists(moved_root / "checkout" / "sentinel"),
               "Removal followed a cache root renamed outside its lineage.");
    }

    fs::remove_all(cache_home / "moguet");
    ValidatedCacheRoot root = prepare_root(cache_home);
    ValidatedCachePath target =
        create_trusted_cache_directory(root, "checkout");
    write_file(target.path() / "sentinel", "child lineage\n");
    fs::path moved_child = tree.path() / "moved-checkout";
    bool mutated = false;
    RemovalHookScope hook([&](std::size_t depth) {
        if(depth != 1 || mutated) return;
        mutated = true;
        fs::rename(target.path(), moved_child);
    });
    expect_cache_error(
        [&]() { remove_trusted_cache_path(target); },
        TrustedCacheErrorCode::ConcurrentReplacement,
        "revoked child lineage removal");
    expect(fs::exists(moved_child / "sentinel"),
           "Removal followed a checkout renamed outside the cache root.");
}

void test_clean_and_rollback_refuse_revoked_authority_lineage() {
    TemporaryTree tree;

    {
        fs::path cache_home = tree.path() / "clean-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath checkout =
            create_trusted_cache_directory(root, "checkout");
        write_file(checkout.path() / "sentinel", "clean original\n");
        PreparedCacheCleanup cleanup =
            preflight_cache_cleanup(root);

        fs::path moved_anchor = tree.path() / "moved-clean-cache-anchor";
        fs::rename(cache_home, moved_anchor);
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        fs::create_directory(cache_home / "moguet");
        fs::permissions(
            cache_home / "moguet", fs::perms::owner_all,
            fs::perm_options::replace);
        write_file(cache_home / "moguet" / "replacement", "replacement\n");

        expect_cache_error(
            [&]() {
                remove_preflighted_cache_paths(std::move(cleanup));
            },
            TrustedCacheErrorCode::ConcurrentReplacement,
            "clean after cache anchor replacement");
        expect(
            fs::exists(
                moved_anchor / "moguet" / "checkout" / "sentinel"),
            "Clean followed the original cache root after its XDG "
            "anchor moved.");
        expect(
            fs::exists(cache_home / "moguet" / "replacement"),
            "Clean removed data from the replacement cache lineage.");
    }

    {
        fs::path authority_parent =
            tree.path() / "rollback-authority-parent";
        fs::path cache_home = authority_parent / "cache-anchor";
        fs::create_directory(authority_parent);
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath checkout =
            create_trusted_cache_directory(root, "checkout");
        write_file(checkout.path() / "partial", "rollback original\n");

        fs::path moved_parent =
            tree.path() / "moved-rollback-authority-parent";
        {
            DirCleanupGuard rollback(checkout);
            fs::rename(authority_parent, moved_parent);
            fs::create_directory(authority_parent);
            fs::create_directory(cache_home);
            fs::permissions(
                cache_home, fs::perms::owner_all,
                fs::perm_options::replace);
            fs::create_directory(cache_home / "moguet");
            fs::permissions(
                cache_home / "moguet", fs::perms::owner_all,
                fs::perm_options::replace);
            fs::create_directory(cache_home / "moguet" / "checkout");
            write_file(
                cache_home / "moguet" / "checkout" / "replacement",
                "replacement\n");
        }

        expect(
            fs::exists(
                moved_parent / "cache-anchor" / "moguet" /
                "checkout" / "partial"),
            "Rollback followed an original checkout whose retained "
            "authority ancestor moved.");
        expect(
            fs::exists(
                cache_home / "moguet" / "checkout" /
                "replacement"),
            "Rollback removed the checkout in a replacement authority "
            "lineage.");
    }
}

void test_cleanup_and_open_reject_unsafe_mode_changes() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);
    ValidatedCachePath checkout =
        create_trusted_cache_directory(root, "checkout");

    fs::create_directory(checkout.path() / "unsafe-nested");
    fs::permissions(
        checkout.path() / "unsafe-nested", fs::perms::all,
        fs::perm_options::replace);
    expect_cache_error(
        [&]() { static_cast<void>(preflight_cache_cleanup(root)); },
        TrustedCacheErrorCode::UnsafePermissions,
        "unsafe nested cleanup preflight");
    expect(fs::is_directory(checkout.path() / "unsafe-nested"),
           "Unsafe nested preflight mutated the tree.");

    fs::permissions(
        checkout.path() / "unsafe-nested", fs::perms::owner_all,
        fs::perm_options::replace);
    fs::permissions(
        checkout.path(), fs::perms::owner_all | fs::perms::group_write,
        fs::perm_options::replace);
    expect_cache_error(
        [&]() {
            WorkDirGuard changed_mode(checkout);
        },
        TrustedCacheErrorCode::UnsafePermissions,
        "post-validation unsafe child mode");
}

void test_staged_rollback_refuses_mode_change() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);

    fs::path staged_path;
    StagedDirectoryHookScope hook([&](const fs::path& path) {
        staged_path = path;
        fs::permissions(
            path,
            fs::perms::owner_all | fs::perms::group_read |
                fs::perms::group_exec,
            fs::perm_options::replace);
    });
    expect_cache_error(
        [&]() {
            static_cast<void>(create_trusted_cache_directory(
                root, "checkout"));
        },
        TrustedCacheErrorCode::UnsafePermissions,
        "staged rollback after mode change");

    expect(!staged_path.empty(), "Staged-directory hook did not run.");
    expect(fs::is_directory(staged_path),
           "Rollback removed a staged directory whose mode changed.");
    expect(path_mode(staged_path) == 0750,
           "Rollback changed the staged directory mode.");
    expect(!fs::exists(root.path() / "checkout"),
           "Failed staged creation published the final checkout.");
}

void test_staged_rollback_refuses_owner_mismatch() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);

    fs::path staged_path;
    std::size_t owner_mismatch_observations = 0;
    {
        StagedOwnerMetadataOverrideScope owner_override;
        StagedDirectoryHookScope hook([&](const fs::path& path) {
            staged_path = path;
            owner_override.enable_for(path);
        });
        expect_cache_error(
            [&]() {
                static_cast<void>(create_trusted_cache_directory(
                    root, "checkout"));
            },
            TrustedCacheErrorCode::OwnershipMismatch,
            "staged rollback after owner mismatch");
        owner_mismatch_observations = owner_override.observations();
    }

    expect(!staged_path.empty(), "Staged-owner hook did not run.");
    expect(owner_mismatch_observations >= 2,
           "Owner mismatch was not observed before and during rollback.");
    expect(fs::is_directory(staged_path),
           "Rollback removed a staged directory whose owner mismatched.");
    expect(path_mode(staged_path) == 0700,
           "Owner-mismatch rollback changed the staged directory mode.");
    expect(!fs::exists(root.path() / "checkout"),
           "Owner-mismatched staged creation published the checkout.");
}

void test_failed_clone_rollback_refuses_replacement() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);
    ValidatedCacheRoot root = prepare_root(cache_home);
    fs::path moved_original = tree.path() / "failed-clone-original";
    fs::path outside = tree.path() / "outside";
    fs::create_directory(outside);
    write_file(outside / "sentinel", "outside\n");

    const std::size_t descriptors_before = open_descriptor_count();
    bool original_failure_preserved = false;
    try {
        ValidatedCachePath clone =
            create_trusted_cache_directory(root, "checkout");
        const PathIdentity original = path_identity(clone.path());
        DirCleanupGuard rollback(clone);
        expect(process_holds_identity(original),
               "Clone rollback guard did not pin its directory at construction.");
        write_file(clone.path() / "partial", "partial clone\n");
        fs::rename(clone.path(), moved_original);
        expect(process_holds_identity(original),
               "Clone rollback guard released a renamed original directory.");
        fs::create_directory_symlink(outside, clone.path());
        throw std::runtime_error("original clone failure");
    } catch(const std::runtime_error& error) {
        original_failure_preserved =
            std::string(error.what()) == "original clone failure";
    }

    expect(original_failure_preserved,
           "Rollback refusal hid the original clone failure.");
    expect(fs::exists(moved_original / "partial"),
           "Rollback followed and deleted the moved original inode.");
    expect(fs::is_symlink(root.path() / "checkout"),
           "Rollback deleted the replacement symlink.");
    expect(fs::exists(outside / "sentinel"),
           "Rollback followed the replacement symlink.");
    expect(open_descriptor_count() == descriptors_before,
           "Refused clone rollback leaked its retained descriptor.");
}

void test_clone_rollback_guard_retained_identity_refusals() {
    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "rename-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath clone =
            create_trusted_cache_directory(root, "checkout");
        write_file(clone.path() / "partial", "original\n");
        const PathIdentity original = path_identity(clone.path());
        const std::size_t descriptors_before = open_descriptor_count();
        fs::path moved_original = tree.path() / "moved-clone";
        {
            DirCleanupGuard rollback(clone);
            expect(process_holds_identity(original),
                   "Rollback guard did not pin a clone before rename.");
            fs::rename(clone.path(), moved_original);
            fs::create_directory(clone.path());
            fs::permissions(
                clone.path(), fs::perms::owner_all,
                fs::perm_options::replace);
            write_file(clone.path() / "replacement", "replacement\n");
        }
        expect(fs::exists(moved_original / "partial"),
               "Rollback followed and removed the renamed original clone.");
        expect(fs::exists(clone.path() / "replacement"),
               "Rollback removed a same-name directory replacement.");
        expect(open_descriptor_count() == descriptors_before,
               "Rename-refused clone rollback leaked its retained descriptor.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "unlink-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath clone =
            create_trusted_cache_directory(root, "checkout");
        const PathIdentity original = path_identity(clone.path());
        const std::size_t descriptors_before = open_descriptor_count();
        {
            DirCleanupGuard rollback(clone);
            expect(fs::remove(clone.path()),
                   "Failed to unlink the original empty clone directory.");
            expect(process_holds_identity(original, 0),
                   "Rollback guard did not retain the unlinked original inode.");
            fs::create_directory(clone.path());
            fs::permissions(
                clone.path(), fs::perms::owner_all,
                fs::perm_options::replace);
            write_file(clone.path() / "replacement", "replacement\n");
        }
        expect(fs::exists(clone.path() / "replacement"),
               "Rollback removed a replacement after original unlink.");
        expect(open_descriptor_count() == descriptors_before,
               "Unlink-refused clone rollback leaked its retained descriptor.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "mode-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath clone =
            create_trusted_cache_directory(root, "checkout");
        const PathIdentity original = path_identity(clone.path());
        const std::size_t descriptors_before = open_descriptor_count();
        StandardOutputCapture warning;
        {
            DirCleanupGuard rollback(clone);
            expect(process_holds_identity(original),
                   "Rollback guard did not retain the mode-change target.");
            fs::permissions(
                clone.path(),
                fs::perms::owner_all | fs::perms::group_read |
                    fs::perms::group_exec,
                fs::perm_options::replace);
        }
        const std::string warning_text = warning.finish();
        expect(fs::is_directory(clone.path()) && path_mode(clone.path()) == 0750,
               "Rollback removed or changed a mode-mutated clone.");
        expect(warning_text.find("Refusing unsafe clone rollback") !=
                   std::string::npos,
               "Mode-change rollback refusal did not emit a warning.");
        expect(warning_text.find(clone.path().string()) == std::string::npos,
               "Rollback refusal warning disclosed the raw cache path.");
        expect(open_descriptor_count() == descriptors_before,
               "Mode-refused clone rollback leaked its retained descriptor.");
    }

    {
        TemporaryTree tree;
        fs::path cache_home = tree.path() / "owner-cache-anchor";
        fs::create_directory(cache_home);
        fs::permissions(
            cache_home, fs::perms::owner_all,
            fs::perm_options::replace);
        ValidatedCacheRoot root = prepare_root(cache_home);
        ValidatedCachePath clone =
            create_trusted_cache_directory(root, "checkout");
        const PathIdentity original = path_identity(clone.path());
        const std::size_t descriptors_before = open_descriptor_count();
        std::size_t owner_mismatch_observations = 0;
        StandardOutputCapture warning;
        {
            StagedOwnerMetadataOverrideScope owner_override;
            {
                DirCleanupGuard rollback(clone);
                expect(process_holds_identity(original),
                       "Rollback guard did not retain the owner-test target.");
                owner_override.enable_for(clone.path());
            }
            owner_mismatch_observations = owner_override.observations();
        }
        const std::string warning_text = warning.finish();
        expect(owner_mismatch_observations >= 1,
               "Owner mismatch seam did not reach clone rollback revalidation.");
        expect(fs::is_directory(clone.path()),
               "Rollback removed a clone with mismatched owner metadata.");
        expect(warning_text.find("Refusing unsafe clone rollback") !=
                   std::string::npos,
               "Owner-mismatch rollback refusal did not emit a warning.");
        expect(warning_text.find(clone.path().string()) == std::string::npos,
               "Owner-mismatch warning disclosed the raw cache path.");
        expect(open_descriptor_count() == descriptors_before,
               "Owner-refused clone rollback leaked its retained descriptor.");
    }
}

void test_new_clean_policy_preserves_legacy_boundary() {
    TemporaryTree tree;
    fs::path cache_home = tree.path() / "cache-anchor";
    fs::create_directory(cache_home);
    fs::permissions(cache_home, fs::perms::owner_all, fs::perm_options::replace);

    fs::path legacy_root = cache_home / "jpacker";
    fs::create_directories(legacy_root / "checkout" / ".git");
    fs::create_directory(legacy_root / ".artifact-workspace~-legacy");
    write_file(legacy_root / "jpacker.log", "legacy log\n");
    write_file(legacy_root / "checkout" / "PKGBUILD", "pkgname=legacy\n");
    write_file(
        legacy_root / ".artifact-workspace~-legacy" / "artifact",
        "legacy artifact\n");
    fs::create_symlink("jpacker.log", legacy_root / "log-link");
    fs::permissions(legacy_root, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    std::vector<TreeEntrySnapshot> legacy_before = snapshot_tree(legacy_root);

    fs::path state_root = tree.path() / "state";
    fs::path config_root = tree.path() / "config";
    fs::create_directories(state_root);
    fs::create_directories(config_root);
    write_file(state_root / "moguet.log", "state log\n");
    write_file(config_root / "config.toml", "config\n");
    TreeEntrySnapshot state_before =
        snapshot_entry(state_root, state_root / "moguet.log");
    TreeEntrySnapshot config_before =
        snapshot_entry(config_root, config_root / "config.toml");

    ValidatedCacheRoot root = prepare_root(cache_home);
    create_trusted_cache_directory(root, "checkout");
    write_file(root.path() / "jpacker.log", "ordinary cache entry\n");
    PreparedCacheCleanup cleanup = preflight_cache_cleanup(root);
    remove_preflighted_cache_paths(std::move(cleanup));

    expect(fs::is_empty(root.path()), "New Moguet cache root is not empty.");
    expect(!fs::exists(root.path() / "jpacker.log"),
           "Legacy-named entry was retained inside the new cache root.");
    expect(snapshot_tree(legacy_root) == legacy_before,
           "Legacy jpacker cache tree changed.");
    expect(snapshot_entry(state_root, state_root / "moguet.log") == state_before,
           "State log changed during cache cleanup.");
    expect(snapshot_entry(config_root, config_root / "config.toml") == config_before,
           "Config tree changed during cache cleanup.");
}

} // namespace

int main() {
    try {
        test_bridge_layout_and_cwd_independence();
        test_prepared_directory_adoption_is_single_consume();
        test_root_and_child_replacement_refusal();
        test_symlink_and_cleanup_preflight();
        test_nested_symlink_cleanup_pins_link_without_following();
        test_cleanup_capability_revalidation_is_global();
        test_repeated_cleanup_preflight_restarts_directory_scan();
        test_cleanup_refuses_held_package_base_lease();
        test_empty_cleanup_capability_is_consumable();
        test_cleanup_capability_reports_fd_exhaustion_before_removal();
        test_cleanup_generation_failures_are_closed();
        test_cleanup_generation_query_mode_is_fixed();
        test_cleanup_refuses_same_stat_different_generation();
        test_cleanup_inventory_changes_preserve_global_barrier();
        test_cleanup_releases_preflight_leases_and_rechecks_busy_targets();
        test_cleanup_late_writer_lease_is_partial_failure();
        test_cleanup_mutation_failures_and_final_pin();
        test_cleanup_large_trees_have_bounded_descriptors();
        test_cleanup_capability_preserves_generation_until_consumed();
        test_removal_plan_rejects_unlinked_replacements();
        test_removal_refuses_revoked_root_and_ancestor_lineage();
        test_clean_and_rollback_refuse_revoked_authority_lineage();
        test_cleanup_and_open_reject_unsafe_mode_changes();
        test_staged_rollback_refuses_mode_change();
        test_staged_rollback_refuses_owner_mismatch();
        test_failed_clone_rollback_refuses_replacement();
        test_clone_rollback_guard_retained_identity_refusals();
        test_new_clean_policy_preserves_legacy_boundary();
        std::cout << "trusted cache tests: all checks passed" << std::endl;
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "trusted cache test failed: " << error.what() << std::endl;
        return 1;
    }
}
