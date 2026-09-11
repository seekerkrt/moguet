#include "reviewed_source_state_store.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

namespace allocation_fault {
bool blocked = false;
std::size_t failures = 0;
} // namespace allocation_fault

// Preserve the replacement boundary under optimized fixture compilation.
[[gnu::noinline]] void* operator new(std::size_t size) {
    if(allocation_fault::blocked) {
        ++allocation_fault::failures;
        throw std::bad_alloc();
    }
    if(void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* memory) noexcept {
    std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace fs = std::filesystem;

namespace {

constexpr std::string_view SHA1_A =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view SHA1_B =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view SHA1_C =
    "cccccccccccccccccccccccccccccccccccccccc";

class TemporaryDirectory final {
    fs::path path_;

public:
    TemporaryDirectory() {
        const fs::path pattern =
            fs::temp_directory_path() / "moguet-reviewed-store-XXXXXX";
        std::string raw = pattern.string();
        if(::mkdtemp(raw.data()) == nullptr) {
            throw std::runtime_error("Failed to create store test directory.");
        }
        path_ = raw;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }
};

class ScopedEnvironmentVariable final {
    std::string name_;
    std::optional<std::string> previous_;

public:
    ScopedEnvironmentVariable(std::string name, const std::string& value)
        : name_(std::move(name)) {
        if(const char* previous = std::getenv(name_.c_str());
           previous != nullptr) {
            previous_ = previous;
        }
        if(::setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("Failed to set store test environment.");
        }
    }

    ~ScopedEnvironmentVariable() noexcept {
        if(previous_.has_value()) {
            static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }
};

class StoreTestHome final {
    TemporaryDirectory temporary_;
    ScopedEnvironmentVariable state_home_;
    ScopedEnvironmentVariable home_;

public:
    StoreTestHome()
        : temporary_(),
          state_home_(
              "XDG_STATE_HOME",
              (temporary_.path() / "state").string()),
          home_("HOME", (temporary_.path() / "home").string()) {
        fs::create_directory(temporary_.path() / "state");
        fs::create_directory(temporary_.path() / "home");
#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        reset_reviewed_source_state_store_test_hooks();
#endif
    }

    const fs::path& root() const noexcept {
        return temporary_.path();
    }
};

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Arm, typename Variant>
const Arm& require_arm(const Variant& value, std::string_view message) {
    const Arm* arm = std::get_if<Arm>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return *arm;
}

PackageBaseIdentity aur_package_base(
    const std::string& package_base = "example-base",
    const std::string& remote =
        "https://aur.archlinux.org/example-base.git") {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(remote)),
        package_base);
}

ReviewedSourceState aur_state(
    const std::string& package_base = "example-base",
    const std::string& remote =
        "https://aur.archlinux.org/example-base.git",
    const std::string& commit = std::string(SHA1_A)) {
    return ReviewedSourceState::make(
        aur_package_base(package_base, remote),
        SourceRevisionIdentity::git_commit(commit));
}

mode_t file_mode(const fs::path& path) {
    struct stat status{};
    if(::lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("lstat failed for " + path.string());
    }
    return status.st_mode & 07777;
}

struct FileIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
};

FileIdentity file_identity(const fs::path& path) {
    struct stat status{};
    if(::lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("lstat failed for " + path.string());
    }
    return FileIdentity{
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino)};
}

struct StatusChangeTime {
    std::intmax_t seconds = 0;
    std::intmax_t nanoseconds = 0;

    bool operator==(const StatusChangeTime&) const = default;
};

StatusChangeTime status_change_time(const fs::path& path) {
    struct stat status{};
    if(::lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("lstat failed for " + path.string());
    }
    return StatusChangeTime{
        static_cast<std::intmax_t>(status.st_ctim.tv_sec),
        static_cast<std::intmax_t>(status.st_ctim.tv_nsec)};
}

ReviewedSourceStateRecordIdentity record_identity_of(const fs::path& path) {
    struct stat status{};
    if(::lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("lstat failed for " + path.string());
    }
    return ReviewedSourceStateRecordIdentity{
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_uid),
        static_cast<std::uintmax_t>(status.st_mode & 07777),
        static_cast<std::uintmax_t>(status.st_nlink),
        static_cast<std::intmax_t>(status.st_size),
        static_cast<std::intmax_t>(status.st_mtim.tv_sec),
        static_cast<std::intmax_t>(status.st_mtim.tv_nsec),
        static_cast<std::intmax_t>(status.st_ctim.tv_sec),
        static_cast<std::intmax_t>(status.st_ctim.tv_nsec)};
}

std::uintmax_t link_count(const fs::path& path) {
    struct stat status{};
    if(::lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error("lstat failed for " + path.string());
    }
    return static_cast<std::uintmax_t>(status.st_nlink);
}

void write_bytes(const fs::path& path, std::string_view contents, mode_t mode) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to open fixture " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error("Failed to write fixture " + path.string());
    }
    if(::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("Failed to chmod fixture " + path.string());
    }
}

// Install contents as a new inode at destination. Used to model non-cooperative
// replacement of an authoritative name.
void replace_path_with_new_inode(
    const fs::path& destination, std::string_view contents, mode_t mode) {
    const fs::path sibling =
        destination.parent_path() /
        (destination.filename().string() + ".replacement");
    write_bytes(sibling, contents, mode);
    fs::rename(sibling, destination);
}

// Rewrite destination through the same directory entry. The test requires the
// inode to stay put so the fixture is a contents-only mutation.
void rewrite_path_in_place(
    const fs::path& destination, std::string_view contents, mode_t mode) {
    const FileIdentity before = file_identity(destination);
    write_bytes(destination, contents, mode);
    const FileIdentity after = file_identity(destination);
    require(before.device == after.device && before.inode == after.inode,
            "In-place rewrite fixture replaced the inode.");
}

void rewrite_path_in_place_restoring_mtime(
    const fs::path& destination, std::string_view contents, mode_t mode) {
    struct stat before{};
    if(::lstat(destination.c_str(), &before) != 0) {
        throw std::runtime_error("lstat failed before mtime-restoring rewrite.");
    }
    rewrite_path_in_place(destination, contents, mode);
    const struct timespec times[2] = {before.st_atim, before.st_mtim};
    if(::utimensat(
           AT_FDCWD, destination.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
        throw std::runtime_error("Failed to restore mtime after rewrite.");
    }
}

void install_package_directory_replacement(
    const fs::path& package_directory, std::string_view marker) {
    const fs::path detached = fs::path(package_directory.string() + ".detached");
    fs::rename(package_directory, detached);
    fs::create_directory(package_directory);
    if(::chmod(package_directory.c_str(), 0700) != 0) {
        throw std::runtime_error("Failed to chmod replacement PackageBase directory.");
    }
    write_bytes(
        package_directory / "-.moguet-reviewed-source-p-new-marker", marker,
        0600);
}

bool is_unsafe_history(const ReviewedSourceStateStoreReadResult& result) {
    return std::holds_alternative<ReviewedSourceStateStoreUnsafeHistory>(result);
}

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<std::string> regular_leaf_names(const fs::path& directory) {
    std::vector<std::string> names;
    if(!fs::exists(directory)) return names;
    for(const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

bool contains_leaf(
    const std::vector<std::string>& names, const std::string& leaf) {
    for(const std::string& name : names) {
        if(name == leaf) return true;
    }
    return false;
}

fs::path origin_path(const PackageBaseIdentity& package_base) {
    return reviewed_source_state_store_entry_path(package_base) /
           reviewed_source_state_store_origin_leaf();
}

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
struct OccupyingPublication {
    std::string contents;
    static OccupyingPublication* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "OccupyingPublication was not armed.");
        write_bytes(context.publication_path, instance->contents, 0600);
    }
};
OccupyingPublication* OccupyingPublication::instance = nullptr;

struct ReplacingCurrent {
    std::string contents;
    static ReplacingCurrent* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "ReplacingCurrent was not armed.");
        require(context.current_path.has_value(),
                "Replacement race had no current generation.");
        replace_path_with_new_inode(
            *context.current_path, instance->contents, 0600);
    }
};
ReplacingCurrent* ReplacingCurrent::instance = nullptr;

struct RewritingCurrentInPlace {
    std::string contents;
    static RewritingCurrentInPlace* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "RewritingCurrentInPlace was not armed.");
        require(context.current_path.has_value(),
                "Same-inode rewrite race had no current generation.");
        rewrite_path_in_place(
            *context.current_path, instance->contents, 0600);
    }
};
RewritingCurrentInPlace* RewritingCurrentInPlace::instance = nullptr;

struct ReplacingCleanupTarget {
    std::string contents;
    fs::path surviving_path;
    static ReplacingCleanupTarget* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "ReplacingCleanupTarget was not armed.");
        instance->surviving_path =
            context.unit_directory / "-.moguet-reviewed-source-planted";
        write_bytes(instance->surviving_path, instance->contents, 0600);
    }
};
ReplacingCleanupTarget* ReplacingCleanupTarget::instance = nullptr;

struct ReplacingTemporarySource {
    std::string contents;
    fs::path decoy_path;
    std::uintmax_t observed_source_device = 0;
    std::uintmax_t observed_source_inode = 0;
    static ReplacingTemporarySource* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "ReplacingTemporarySource was not armed.");
        require(context.temporary_leaf.empty(),
                "Descriptor-bound publication still used a named temp source.");
        instance->observed_source_device = context.source_device;
        instance->observed_source_inode = context.source_inode;
        instance->decoy_path =
            context.unit_directory / "-.moguet-reviewed-source-foreign";
        write_bytes(instance->decoy_path, instance->contents, 0600);
        replace_path_with_new_inode(
            instance->decoy_path, instance->contents, 0600);
    }
};
ReplacingTemporarySource* ReplacingTemporarySource::instance = nullptr;

struct RewritingTemporarySourceInPlace {
    std::string contents;
    fs::path decoy_path;
    std::uintmax_t observed_source_device = 0;
    std::uintmax_t observed_source_inode = 0;
    static RewritingTemporarySourceInPlace* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr,
                "RewritingTemporarySourceInPlace was not armed.");
        require(context.temporary_leaf.empty(),
                "Descriptor-bound publication still used a named temp source.");
        instance->observed_source_device = context.source_device;
        instance->observed_source_inode = context.source_inode;
        instance->decoy_path =
            context.unit_directory / "-.moguet-reviewed-source-rewrite";
        write_bytes(instance->decoy_path, std::string(instance->contents.size(), 'x'), 0600);
        rewrite_path_in_place_restoring_mtime(
            instance->decoy_path, instance->contents, 0600);
    }
};
RewritingTemporarySourceInPlace* RewritingTemporarySourceInPlace::instance =
    nullptr;

struct ReplacingPackageDirectory {
    std::string marker;
    static ReplacingPackageDirectory* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "ReplacingPackageDirectory was not armed.");
        install_package_directory_replacement(
            context.unit_directory, instance->marker);
    }
};
ReplacingPackageDirectory* ReplacingPackageDirectory::instance = nullptr;

// Mutate an already-proven ancestor at an exact protocol boundary. The path is
// captured before the operation starts, so no readdir order or timing is
// involved.
struct MutatingAncestor {
    fs::path target;
    std::string contents;
    bool should_replace_inode = false;
    static MutatingAncestor* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext&) {
        require(instance != nullptr, "MutatingAncestor was not armed.");
        if(instance->should_replace_inode) {
            replace_path_with_new_inode(
                instance->target, instance->contents, 0600);
        } else {
            rewrite_path_in_place(
                instance->target, instance->contents, 0600);
        }
    }
};
MutatingAncestor* MutatingAncestor::instance = nullptr;

// Add a managed entry to the PackageBase directory after its inventory was
// already proven: a same-generation fork, a future-owned successor, or an
// unrecognized managed name.
struct PlantingManagedLeaf {
    std::string leaf;
    std::string contents;
    static PlantingManagedLeaf* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr, "PlantingManagedLeaf was not armed.");
        write_bytes(
            context.unit_directory / instance->leaf, instance->contents,
            0600);
    }
};
PlantingManagedLeaf* PlantingManagedLeaf::instance = nullptr;

// Replace the named .../reviewed-sources/aur/ store directory itself and build
// a fresh PackageBase directory underneath it, leaving the operation holding a
// detached S-old / P-old pair.
struct ReplacingStoreDirectory {
    std::string package_leaf;
    std::string marker;
    fs::path new_package_directory;
    static ReplacingStoreDirectory* instance;

    static void handler(const ReviewedSourceStateStoreTestRaceContext&) {
        require(instance != nullptr, "ReplacingStoreDirectory was not armed.");
        const fs::path store = reviewed_source_state_store_directory();
        fs::rename(store, fs::path(store.string() + ".detached"));
        fs::create_directory(store);
        if(::chmod(store.c_str(), 0700) != 0) {
            throw std::runtime_error("Failed to chmod replacement aur store.");
        }
        instance->new_package_directory = store / instance->package_leaf;
        fs::create_directory(instance->new_package_directory);
        if(::chmod(instance->new_package_directory.c_str(), 0700) != 0) {
            throw std::runtime_error("Failed to chmod replacement PackageBase.");
        }
        write_bytes(
            instance->new_package_directory /
                "-.moguet-reviewed-source-s-new-marker",
            instance->marker, 0600);
    }
};
ReplacingStoreDirectory* ReplacingStoreDirectory::instance = nullptr;

// M411-S2-011. Add a second name for one chain record while the operation is
// inside that record's read: the bytes are already in hand, the post-read
// status proof has not run. The alias is created outside the PackageBase
// directory on the same filesystem, so the PackageBase inventory, the leaf
// names, the inode, the size, and the content digest are all unchanged. Only
// the record's own st_nlink reports the mutation.
struct LinkingRecordOutsidePackage {
    std::string target_leaf;
    fs::path alias_path;
    fs::path linked_record;
    bool did_link = false;
    static LinkingRecordOutsidePackage* instance;

    // Armed from an outer boundary race point so the inner window belongs to
    // the final reproof rather than to the operation's first chain proof.
    static void arm(const ReviewedSourceStateStoreTestRaceContext&) {
        require(instance != nullptr,
                "LinkingRecordOutsidePackage was not armed.");
        run_reviewed_source_state_store_race_once_for_test(
            ReviewedSourceStateStoreTestRacePoint::AfterRecordContentsRead,
            &LinkingRecordOutsidePackage::handler);
    }

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr,
                "LinkingRecordOutsidePackage was not armed.");
        require(context.record_path.has_value(),
                "Record read race carried no record path.");
        if(context.record_path->filename().string() != instance->target_leaf) {
            // Not the record under test. The hook is one-shot, so re-arm it for
            // the next record the same reproof reads.
            run_reviewed_source_state_store_race_once_for_test(
                ReviewedSourceStateStoreTestRacePoint::
                    AfterRecordContentsRead,
                &LinkingRecordOutsidePackage::handler);
            return;
        }
        if(::link(context.record_path->c_str(),
                  instance->alias_path.c_str()) != 0) {
            throw std::runtime_error(
                "Failed to create the external hardlink alias.");
        }
        instance->linked_record = *context.record_path;
        instance->did_link = true;
    }
};
LinkingRecordOutsidePackage* LinkingRecordOutsidePackage::instance = nullptr;

// M411-S2-012. Preserve the opened inode through an alias outside the
// PackageBase directory, then atomically replace the exact generation leaf
// with a pre-created inode. The old inode returns to nlink == 1, the new named
// inode also has nlink == 1, and the leaf inventory never changes.
struct RebindingRecordOutsidePackage {
    std::string target_leaf;
    fs::path alias_path;
    fs::path replacement_path;
    fs::path rebound_record;
    bool did_rebind = false;
    static RebindingRecordOutsidePackage* instance;

    static void arm(const ReviewedSourceStateStoreTestRaceContext&) {
        require(instance != nullptr,
                "RebindingRecordOutsidePackage was not armed.");
        run_reviewed_source_state_store_race_once_for_test(
            ReviewedSourceStateStoreTestRacePoint::AfterRecordContentsRead,
            &RebindingRecordOutsidePackage::handler);
    }

    static void handler(const ReviewedSourceStateStoreTestRaceContext& context) {
        require(instance != nullptr,
                "RebindingRecordOutsidePackage was not armed.");
        require(context.record_path.has_value(),
                "Record rebind race carried no record path.");
        if(context.record_path->filename().string() != instance->target_leaf) {
            run_reviewed_source_state_store_race_once_for_test(
                ReviewedSourceStateStoreTestRacePoint::
                    AfterRecordContentsRead,
                &RebindingRecordOutsidePackage::handler);
            return;
        }
        if(::link(context.record_path->c_str(),
                  instance->alias_path.c_str()) != 0) {
            throw std::runtime_error(
                "Failed to preserve the opened inode through an external alias.");
        }
        fs::rename(instance->replacement_path, *context.record_path);
        instance->rebound_record = *context.record_path;
        instance->did_rebind = true;
    }
};
RebindingRecordOutsidePackage* RebindingRecordOutsidePackage::instance =
    nullptr;
#endif

void test_missing_lookup_does_not_create() {
    StoreTestHome home;
    const PackageBaseIdentity expected = aur_package_base();
    const auto result = read_reviewed_source_state(expected);
    const auto& loaded = require_arm<ReviewedSourceStateStoreRead>(
        result, "Missing lookup was not a store read.");
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                loaded.observation),
            "Absent store was not Missing.");
    require(!loaded.observed.has_value(), "Missing lookup carried bytes.");
    require(!fs::exists(home.root() / "state" / "moguet"),
            "Lookup created the XDG state application directory.");
}

void test_first_create_is_0600_and_round_trips() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "First publication failed.");
    require(published.state == state, "Published state drifted.");
    require(published.observed.raw_contents ==
                encode_reviewed_source_state(state),
            "Publication did not write canonical contents.");
    require(published.observed.generation == 1,
            "First publication was not generation 1.");
    require(published.observed.leaf_name ==
                reviewed_source_state_store_origin_leaf(),
            "First publication leaf drifted.");

    const fs::path package_dir =
        reviewed_source_state_store_entry_path(state.package_base());
    const fs::path origin = origin_path(state.package_base());
    require(file_mode(origin) == 0600, "Published file mode was not 0600.");
    require(file_mode(package_dir) == 0700,
            "Package directory mode was not 0700.");
    require(file_mode(package_dir.parent_path()) == 0700,
            "Store directory mode was not 0700.");
    require(read_bytes(origin) == published.observed.raw_contents,
            "On-disk bytes drifted from observed publication.");

    const auto read_back = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(state.package_base()),
        "Read after publish failed.");
    const auto& loaded = require_arm<ReviewedSourceStateLoaded>(
        read_back.observation, "Published state was not Loaded.");
    require(loaded.state == state, "Read-back lost reviewed state.");
    require(read_back.observed.has_value() &&
                read_back.observed->raw_contents ==
                    published.observed.raw_contents,
            "Read-back lost observed raw bytes.");
}

void test_successor_leaf_binds_inode_and_content_digest() {
    ReviewedSourceStateRecordIdentity identity;
    identity.device = 8;
    identity.inode = 16;
    const std::string empty_leaf = reviewed_source_state_store_successor_leaf(
        2, identity, "");
    require(empty_leaf ==
                "2.8-16-0-0.e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855.toml",
            "Successor leaf did not bind SHA-256 of empty predecessor contents.");
    const std::string abc_leaf = reviewed_source_state_store_successor_leaf(
        3, identity, "abc");
    require(abc_leaf ==
                "3.8-16-0-0.ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad.toml",
            "Successor leaf did not bind SHA-256 of predecessor contents.");
    require(empty_leaf != abc_leaf,
            "Content digest was omitted from generation identity.");
    identity.status_change_time_seconds = 11;
    identity.status_change_time_nanoseconds = 22;
    require(reviewed_source_state_store_successor_leaf(2, identity, "") !=
                empty_leaf,
            "Successor leaf omitted predecessor status-change time.");
    const std::string padded_55(55, 'a');
    const std::string padded_56 =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::string block_64(64, 'a');
    const std::string multi_block(65, 'a');
    identity.status_change_time_seconds = 0;
    identity.status_change_time_nanoseconds = 0;
    require(reviewed_source_state_store_successor_leaf(2, identity, padded_55) ==
                "2.8-16-0-0.9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318.toml",
            "SHA-256 55-byte known vector drifted.");
    require(reviewed_source_state_store_successor_leaf(2, identity, padded_56) ==
                "2.8-16-0-0.248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1.toml",
            "SHA-256 56-byte known vector drifted.");
    require(reviewed_source_state_store_successor_leaf(2, identity, block_64) ==
                "2.8-16-0-0.ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb.toml",
            "SHA-256 64-byte known vector drifted.");
    require(reviewed_source_state_store_successor_leaf(2, identity, multi_block) ==
                "2.8-16-0-0.635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0.toml",
            "SHA-256 multi-block known vector drifted.");
}

void test_cas_replacement_and_stale_writer() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));

    const auto published_first = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Baseline publication failed.");
    const auto published_second = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(second, published_first.observed),
        "Valid CAS replacement failed.");
    require(published_second.state == second, "CAS replacement lost new state.");
    require(published_second.observed.generation == 2,
            "Successor publication was not generation 2.");

    const fs::path package_dir =
        reviewed_source_state_store_entry_path(second.package_base());
    const fs::path successor =
        package_dir / published_second.observed.leaf_name;
    require(read_bytes(successor) == published_second.observed.raw_contents,
            "Successor generation lost B's bytes.");
    require(read_bytes(origin_path(first.package_base())) ==
                published_first.observed.raw_contents,
            "CAS successor mutated the origin generation.");

    const auto stale = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(first, published_first.observed),
        "Stale writer was not a store failure.");
    require(stale.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Stale writer was not ConcurrentReplacement.");
    require(read_bytes(successor) == published_second.observed.raw_contents,
            "Stale writer mutated B's authoritative generation.");

    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(second.package_base()),
        "Post-conflict read failed.");
    const auto& loaded = require_arm<ReviewedSourceStateLoaded>(
        current.observation, "B's state was not preserved.");
    require(loaded.state == second, "Stale writer rolled back reviewed state.");
}

void test_missing_create_is_no_replace() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));

    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "First creator failed.");
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(second, std::nullopt),
        "Second Missing-create was not a failure.");
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Second creator was not ConcurrentReplacement.");

    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post no-replace read failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "First creator state was lost.")
                    .state == first,
            "No-replace create overwrote the first publication.");
    require(read_bytes(origin_path(first.package_base())) ==
                encode_reviewed_source_state(first),
            "Second Missing-create mutated the origin generation.");
}

void test_raw_byte_guard_is_not_reencoded() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Canonical publication failed.");

    const fs::path origin = origin_path(state.package_base());
    const std::string equivalent =
        "reviewed_commit = \"" + std::string(SHA1_A) +
        "\"\n"
        "# semantically equal, different bytes\n"
        "canonical_git_remote = \"https://aur.archlinux.org/example-base.git\"\n"
        "package_base = \"example-base\"\n"
        "source_kind = \"aur\"\n"
        "schema_version = 1\n";
    write_bytes(origin, equivalent, 0600);

    const auto reread = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(state.package_base()),
        "Equivalent-document read failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                reread.observation, "Equivalent document was not Loaded.")
                    .state == state,
            "Equivalent layout changed typed state.");
    require(reread.observed.has_value() &&
                reread.observed->raw_contents == equivalent,
            "Read did not retain observed raw bytes.");

    const auto stale = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(state, published.observed),
        "Re-encode CAS guard was accepted.");
    require(stale.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Raw-byte mismatch was not ConcurrentReplacement.");
    require(read_bytes(origin) == equivalent,
            "Re-encode publication overwrote observed bytes.");
}

void test_future_schema_is_not_overwritten() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Seed publication failed.");

    const fs::path origin = origin_path(state.package_base());
    const std::string future = "schema_version = 2\nnext_field = true\n";
    write_bytes(origin, future, 0600);

    const auto read_future = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(state.package_base()),
        "Future schema read failed.");
    require(std::holds_alternative<ReviewedSourceStateUnsupportedFuture>(
                read_future.observation),
            "Future schema was not UnsupportedFuture.");
    require(read_future.observed.has_value() &&
                read_future.observed->raw_contents == future,
            "Future schema raw bytes were lost.");

    const auto refused = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(state, read_future.observed),
        "Future overwrite was not a failure.");
    require(refused.kind ==
                ReviewedSourceStateStoreFailureKind::
                    FutureSchemaOverwriteRefused,
            "Future schema overwrite was not refused.");
    require(read_bytes(origin) == future,
            "Future schema bytes were overwritten.");
}

void test_source_mismatch_is_not_loaded() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state(
        "example-base",
        "https://aur.archlinux.org/other-name.git");
    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Mismatch seed publication failed.");

    const auto read_back = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(aur_package_base()),
        "Mismatch lookup failed.");
    require(std::holds_alternative<ReviewedSourceStateSourceMismatch>(
                read_back.observation),
            "Different remote in PackageBase directory was treated as Loaded.");
}

void test_invalid_and_corrupted_are_not_missing() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Seed publication failed.");
    const fs::path origin = origin_path(state.package_base());

    write_bytes(origin, "schema_version = 1\n", 0600);
    const auto invalid = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(state.package_base()),
        "Invalid document read failed.");
    require(std::holds_alternative<ReviewedSourceStateInvalid>(
                invalid.observation),
            "Invalid current-schema document was flattened.");

    write_bytes(origin, "schema_version = [\n", 0600);
    const auto corrupted = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(state.package_base()),
        "Corrupted document read failed.");
    require(std::holds_alternative<ReviewedSourceStateCorrupted>(
                corrupted.observation),
            "Corrupted document was flattened to Missing.");
}

void test_symlink_hardlink_and_mode_are_failures() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Seed publication failed.");
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(state.package_base());
    const fs::path origin = origin_path(state.package_base());
    const fs::path sibling = package_dir / "other.toml";

    fs::remove(origin);
    fs::create_symlink(sibling, origin);
    const auto symlink = require_arm<ReviewedSourceStateStoreFailure>(
        read_reviewed_source_state(state.package_base()),
        "Symlink was not a store failure.");
    require(symlink.kind ==
                ReviewedSourceStateStoreFailureKind::UnsupportedFileType,
            "Symlink was flattened to Missing.");
    require(symlink.observed_file_type == fs::file_type::symlink,
            "Symlink type was lost.");
    fs::remove(origin);

    write_bytes(origin, encode_reviewed_source_state(state), 0600);
    write_bytes(sibling, "other", 0600);
    if(::link(sibling.c_str(), (origin.string() + ".link").c_str()) != 0) {
        throw std::runtime_error("Failed to create hardlink fixture.");
    }
    fs::remove(origin);
    if(::link((origin.string() + ".link").c_str(), origin.c_str()) != 0) {
        throw std::runtime_error("Failed to install hardlinked destination.");
    }
    const auto hardlink = require_arm<ReviewedSourceStateStoreFailure>(
        read_reviewed_source_state(state.package_base()),
        "Hardlink was not a store failure.");
    require(hardlink.kind ==
                ReviewedSourceStateStoreFailureKind::MultipleHardLinks,
            "Hardlink count violation was flattened.");
    fs::remove(origin);
    fs::remove(origin.string() + ".link");
    fs::remove(sibling);

    write_bytes(origin, encode_reviewed_source_state(state), 0644);
    const auto mode = require_arm<ReviewedSourceStateStoreFailure>(
        read_reviewed_source_state(state.package_base()),
        "0644 file was not a store failure.");
    require(mode.kind ==
                ReviewedSourceStateStoreFailureKind::UnsafePermissions,
            "Unsafe mode was flattened to Missing.");

    fs::remove(origin);
    if(::mkfifo(origin.c_str(), 0600) != 0) {
        throw std::runtime_error("Failed to create FIFO fixture.");
    }
    const auto fifo = require_arm<ReviewedSourceStateStoreFailure>(
        read_reviewed_source_state(state.package_base()),
        "FIFO was not a store failure.");
    require(fifo.kind ==
                ReviewedSourceStateStoreFailureKind::UnsupportedFileType,
            "FIFO was flattened to Missing.");
}

void test_oversized_record_is_typed_failure() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Seed publication failed.");
    const fs::path origin = origin_path(state.package_base());
    write_bytes(
        origin,
        std::string(reviewed_source_state_store_max_record_bytes + 1, 'x'),
        0600);
    const auto oversized = require_arm<ReviewedSourceStateStoreFailure>(
        read_reviewed_source_state(state.package_base()),
        "Oversized record was not a store failure.");
    require(oversized.kind ==
                ReviewedSourceStateStoreFailureKind::RecordTooLarge,
            "Oversized record was not RecordTooLarge.");
}

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
void test_fsync_and_rename_failures_preserve_destination() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path origin = origin_path(first.package_base());
    const std::string original = read_bytes(origin);
    const FileIdentity original_identity = file_identity(origin);

    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::Sync);
    const auto sync_failed = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(second, published.observed),
        "Injected fsync failure was not reported.");
    require(sync_failed.kind ==
                ReviewedSourceStateStoreFailureKind::SyncFailed,
            "Injected fsync failure kind drifted.");
    require(read_bytes(origin) == original,
            "File fsync failure mutated the published destination.");
    require(file_identity(origin).inode == original_identity.inode,
            "File fsync failure replaced the origin inode.");

    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::Rename);
    const auto rename_failed = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(second, published.observed),
        "Injected rename failure was not reported.");
    require(rename_failed.kind ==
                ReviewedSourceStateStoreFailureKind::RenameFailed,
            "Injected rename failure kind drifted.");
    require(read_bytes(origin) == original,
            "rename failure mutated the published destination.");
    require(file_identity(origin).inode == original_identity.inode,
            "rename failure replaced the origin inode.");
}

void test_directory_fsync_failure_is_published_uncertain() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::DirectorySync);
    const auto uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            publish_reviewed_source_state(second, published.observed),
            "Directory fsync failure was flattened to a pre-commit failure.");
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    DirectorySyncUncertain,
            "Directory fsync failure issue drifted.");
    require(uncertain.failure_kind ==
                ReviewedSourceStateStoreFailureKind::SyncFailed,
            "Directory fsync failure kind drifted.");
    require(uncertain.observed.has_value(),
            "Directory fsync failure lost the published token.");
    require(uncertain.observed->raw_contents ==
                encode_reviewed_source_state(second),
            "Directory fsync failure lost the new state bytes.");

    const fs::path successor =
        reviewed_source_state_store_entry_path(second.package_base()) /
        uncertain.observed->leaf_name;
    require(read_bytes(successor) == encode_reviewed_source_state(second),
            "Directory fsync failure did not leave the new generation authoritative.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(second.package_base()),
        "Post directory-fsync-failure read failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "New generation was not Loaded.")
                    .state == second,
            "Directory fsync failure hid the published state.");
}

void test_post_commit_verify_failure_is_published_uncertain() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::PostCommitVerify);
    const auto uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            publish_reviewed_source_state(second, published.observed),
            "Post-commit verify failure was flattened.");
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    PublishedIdentityUncertain,
            "Post-commit verify issue drifted.");
    require(uncertain.observed.has_value(),
            "Post-commit verify failure lost the published token.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(second.package_base()) /
        uncertain.observed->leaf_name;
    require(read_bytes(successor) == encode_reviewed_source_state(second),
            "Post-commit verify failure rolled back the new generation.");
}

void test_publication_boundary_occupancy_preserves_replacement() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string replacement = encode_reviewed_source_state(second);
    OccupyingPublication occupier{replacement};
    OccupyingPublication::instance = &occupier;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &OccupyingPublication::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_C)),
            published.observed),
        "Occupied successor was not a conflict.");
    OccupyingPublication::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Occupied successor was not ConcurrentReplacement.");

    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    const fs::path successor =
        package_dir /
        reviewed_source_state_store_successor_leaf(
            2, published.observed.identity,
            published.observed.raw_contents);
    require(read_bytes(successor) == replacement,
            "A displaced B from the successor name.");
    require(read_bytes(origin_path(first.package_base())) ==
                published.observed.raw_contents,
            "Occupied-successor race mutated the origin generation.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post occupancy read failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "Replacement was not Loaded.")
                    .state == second,
            "Occupied successor did not remain authoritative.");
}

void test_publication_boundary_inode_replacement_preserves_b() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const FileIdentity original_identity =
        file_identity(origin_path(first.package_base()));
    const ReviewedSourceState replacement_state = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string replacement =
        encode_reviewed_source_state(replacement_state);
    ReplacingCurrent replacer{replacement};
    ReplacingCurrent::instance = &replacer;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &ReplacingCurrent::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_C)),
            published.observed),
        "Replaced destination was not a conflict.");
    ReplacingCurrent::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Replaced destination was not ConcurrentReplacement.");

    const fs::path origin = origin_path(first.package_base());
    require(read_bytes(origin) == replacement,
            "A displaced B's replacement bytes from the origin name.");
    require(file_identity(origin).inode != original_identity.inode,
            "Replacement fixture did not install a new inode.");
    // The pre-commit authority reproof observes the replaced origin before the
    // kernel commit point, so no successor is linked at all. The store is left
    // holding exactly B's replacement as a complete single-generation chain
    // instead of A's orphan successor.
    const std::string successor_leaf =
        reviewed_source_state_store_successor_leaf(
            2, published.observed.identity,
            published.observed.raw_contents);
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        successor_leaf;
    require(!fs::exists(successor),
            "Pre-commit refusal still linked an orphan successor.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post origin-replacement lookup failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "B's replacement was not Loaded.")
                    .state == replacement_state,
            "A's refused publication displaced B's replacement authority.");
}

void test_future_schema_boundary_preserves_future_bytes() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const std::string future = "schema_version = 2\nnext_field = true\n";
    OccupyingPublication occupier{future};
    OccupyingPublication::instance = &occupier;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &OccupyingPublication::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_B)),
            published.observed),
        "Future occupancy was not a conflict.");
    OccupyingPublication::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Future occupancy was not ConcurrentReplacement.");

    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        reviewed_source_state_store_successor_leaf(
            2, published.observed.identity,
            published.observed.raw_contents);
    require(read_bytes(successor) == future,
            "Future schema bytes were displaced from the successor name.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post future-occupancy read failed.");
    require(std::holds_alternative<ReviewedSourceStateUnsupportedFuture>(
                current.observation),
            "Future occupancy was not the authoritative observation.");
    require(current.observed.has_value() &&
                current.observed->raw_contents == future,
            "Future occupancy lost raw bytes.");
}

void test_cleanup_replacement_is_not_deleted() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string replacement = "replacement-cleanup-target\n";
    ReplacingCleanupTarget replacer{replacement, {}};
    ReplacingCleanupTarget::instance = &replacer;
    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::Sync);
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::BeforeCleanup,
        &ReplacingCleanupTarget::handler);
    const auto failed = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(second, published.observed),
        "Cleanup race was not a pre-publication failure.");
    ReplacingCleanupTarget::instance = nullptr;
    require(failed.kind == ReviewedSourceStateStoreFailureKind::SyncFailed,
            "Cleanup race flattened the injected file-sync failure.");
    require(fs::exists(replacer.surviving_path),
            "Cleanup race deleted the replacement path.");
    require(read_bytes(replacer.surviving_path) == replacement,
            "Cleanup race deleted or mutated the planted residue bytes.");
    require(read_bytes(origin_path(first.package_base())) ==
                published.observed.raw_contents,
            "Cleanup race mutated the authoritative origin.");
    require(!failed.leftover_artifact.has_value() ||
                !fs::exists(*failed.leftover_artifact) ||
                read_bytes(*failed.leftover_artifact) == replacement,
            "Pre-commit failure unlinked a planted named residue.");
}

void test_missing_missing_boundary_preserves_first_writer() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string first_bytes = encode_reviewed_source_state(first);
    OccupyingPublication occupier{first_bytes};
    OccupyingPublication::instance = &occupier;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &OccupyingPublication::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(second, std::nullopt),
        "Missing/Missing occupancy was not a conflict.");
    OccupyingPublication::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Missing/Missing occupancy was not ConcurrentReplacement.");
    require(read_bytes(origin_path(first.package_base())) == first_bytes,
            "Second Missing publisher overwrote the first origin.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post Missing/Missing read failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "First Missing publisher was lost.")
                    .state == first,
            "First Missing publisher did not remain authoritative.");
}

void test_loaded_loaded_boundary_preserves_newer_writer() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState newer = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string newer_bytes = encode_reviewed_source_state(newer);
    OccupyingPublication occupier{newer_bytes};
    OccupyingPublication::instance = &occupier;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &OccupyingPublication::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_C)),
            published.observed),
        "Loaded/Loaded occupancy was not a conflict.");
    OccupyingPublication::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Loaded/Loaded occupancy was not ConcurrentReplacement.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        reviewed_source_state_store_successor_leaf(
            2, published.observed.identity,
            published.observed.raw_contents);
    require(read_bytes(successor) == newer_bytes,
            "Stale writer displaced the newer successor.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post Loaded/Loaded read failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "Newer writer was not Loaded.")
                    .state == newer,
            "Stale writer rolled back the newer reviewed state.");
}

void test_publication_boundary_same_inode_rewrite_preserves_b() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path origin = origin_path(first.package_base());
    const FileIdentity original_identity = file_identity(origin);
    const ReviewedSourceState rewritten_state = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string rewritten = encode_reviewed_source_state(rewritten_state);
    RewritingCurrentInPlace rewriter{rewritten};
    RewritingCurrentInPlace::instance = &rewriter;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &RewritingCurrentInPlace::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_C)),
            published.observed),
        "Same-inode rewrite was not a conflict.");
    RewritingCurrentInPlace::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Same-inode rewrite was not ConcurrentReplacement.");
    require(read_bytes(origin) == rewritten,
            "A displaced B's rewritten bytes from the origin name.");
    require(file_identity(origin).device == original_identity.device &&
                file_identity(origin).inode == original_identity.inode,
            "Same-inode rewrite fixture did not keep the origin inode.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        reviewed_source_state_store_successor_leaf(
            2, published.observed.identity,
            published.observed.raw_contents);
    require(!fs::exists(successor),
            "Pre-commit refusal still linked an orphan successor.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post same-inode rewrite lookup failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "B's rewrite was not Loaded.")
                    .state == rewritten_state,
            "A's refused publication displaced B's rewritten authority.");
}

void test_publication_boundary_same_inode_future_rewrite_is_not_downgraded() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path origin = origin_path(first.package_base());
    const FileIdentity original_identity = file_identity(origin);
    const std::string future = "schema_version = 2\nnext_field = true\n";
    RewritingCurrentInPlace rewriter{future};
    RewritingCurrentInPlace::instance = &rewriter;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &RewritingCurrentInPlace::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_B)),
            published.observed),
        "Same-inode future rewrite was not a conflict.");
    RewritingCurrentInPlace::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Same-inode future rewrite was not ConcurrentReplacement.");
    require(read_bytes(origin) == future,
            "A displaced future-schema bytes from the origin name.");
    require(file_identity(origin).device == original_identity.device &&
                file_identity(origin).inode == original_identity.inode,
            "Future in-place rewrite replaced the origin inode.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        reviewed_source_state_store_successor_leaf(
            2, published.observed.identity,
            published.observed.raw_contents);
    require(!fs::exists(successor),
            "Pre-commit refusal linked a successor over future-schema bytes.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post future in-place rewrite lookup failed.");
    require(std::holds_alternative<ReviewedSourceStateUnsupportedFuture>(
                current.observation),
            "Future-schema origin was downgraded after the refused publication.");
    require(read_bytes(origin) == future,
            "Refused publication mutated the future-schema origin bytes.");
}

void test_internal_temp_is_not_authoritative() {
    StoreTestHome home;
    const ReviewedSourceState state = aur_state();
    require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(state, std::nullopt),
        "Seed publication failed.");
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(state.package_base());
    write_bytes(
        package_dir / "-.moguet-reviewed-source-999-1",
        "schema_version = 2\nstale_temp = true\n", 0600);
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(state.package_base()),
        "Lookup with leftover temp failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "Leftover temp changed authority.")
                    .state == state,
            "Internal temp was treated as authoritative.");
    require(contains_leaf(
                regular_leaf_names(package_dir),
                "-.moguet-reviewed-source-999-1"),
            "Lookup unlinked an unpublished temp.");
}

struct ThreeGenerationFixture {
    ReviewedSourceState first = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    ReviewedSourceState third = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_C));
    std::optional<ReviewedSourceStateStorePublished> published_first;
    std::optional<ReviewedSourceStateStorePublished> published_second;
    std::optional<ReviewedSourceStateStorePublished> published_third;
    fs::path package_dir;
    fs::path origin;
    fs::path generation2;
    fs::path generation3;
};

ThreeGenerationFixture publish_three_generations() {
    ThreeGenerationFixture fixture;
    fixture.published_first = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(fixture.first, std::nullopt),
        "Generation 1 publication failed.");
    fixture.published_second = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(
            fixture.second, fixture.published_first->observed),
        "Generation 2 publication failed.");
    fixture.published_third = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(
            fixture.third, fixture.published_second->observed),
        "Generation 3 publication failed.");
    fixture.package_dir = reviewed_source_state_store_entry_path(
        fixture.first.package_base());
    fixture.origin = origin_path(fixture.first.package_base());
    fixture.generation2 = fixture.package_dir /
                          fixture.published_second->observed.leaf_name;
    fixture.generation3 = fixture.package_dir /
                          fixture.published_third->observed.leaf_name;
    return fixture;
}

void require_higher_generation_fail_closed(
    const ThreeGenerationFixture& fixture, const std::string& mutated_path_bytes,
    const fs::path& mutated_path, const char* message) {
    const auto result = read_reviewed_source_state(fixture.first.package_base());
    require(is_unsafe_history(result), message);
    const auto& history = require_arm<ReviewedSourceStateStoreUnsafeHistory>(
        result, message);
    require(history.issue ==
                    ReviewedSourceStateStoreHistoryIssue::
                        HigherGenerationUnreachable ||
                history.issue ==
                    ReviewedSourceStateStoreHistoryIssue::OrphanSuccessor ||
                history.issue ==
                    ReviewedSourceStateStoreHistoryIssue::ForkDetected,
            message);
    require(history.highest_generation == 3,
            "Lookup hid leftover generation 3.");
    require(fs::exists(fixture.generation2), "Generation 2 was removed.");
    require(fs::exists(fixture.generation3), "Generation 3 was removed.");
    require(read_bytes(mutated_path) == mutated_path_bytes,
            "Mutation fixture lost the injected bytes.");
}

void test_publication_boundary_temp_replacement_does_not_publish_foreign_bytes() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string publication = encode_reviewed_source_state(second);
    // The decoy must not encode what A intends to publish, otherwise matching
    // bytes on disk prove nothing about which inode the kernel linked.
    const std::string foreign = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_C)));
    require(foreign != publication, "Decoy bytes matched A's publication.");
    ReplacingTemporarySource replacer{foreign, {}, 0, 0};
    ReplacingTemporarySource::instance = &replacer;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &ReplacingTemporarySource::handler);
    const auto published_second =
        require_arm<ReviewedSourceStateStorePublished>(
            publish_reviewed_source_state(second, published.observed),
            "Descriptor-bound publication failed after named decoy replacement.");
    ReplacingTemporarySource::instance = nullptr;
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        published_second.observed.leaf_name;
    require(read_bytes(successor) == publication,
            "Named temp replacement published foreign bytes.");
    require(read_bytes(successor) != foreign,
            "Published record carried the decoy bytes.");
    require(replacer.observed_source_inode != 0,
            "Publication source inode was not exposed to the race hook.");
    const FileIdentity successor_identity = file_identity(successor);
    require(successor_identity.device == replacer.observed_source_device &&
                successor_identity.inode == replacer.observed_source_inode,
            "Published name did not resolve to the retained O_TMPFILE inode.");
    require(published_second.observed.identity.device ==
                    successor_identity.device &&
                published_second.observed.identity.inode ==
                    successor_identity.inode,
            "Result token identity disagreed with the on-disk successor.");
    const FileIdentity decoy_identity = file_identity(replacer.decoy_path);
    require(decoy_identity.inode != successor_identity.inode,
            "Publication tracked the replaced decoy inode.");
    require(read_bytes(replacer.decoy_path) == foreign,
            "Foreign decoy was consumed as the publication source.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post temp-replacement lookup failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                current.observation, "A publication was not Loaded.")
                    .state == second,
            "Descriptor-bound publication did not become authority.");
    require(current.observed.has_value() &&
                current.observed->leaf_name ==
                    published_second.observed.leaf_name &&
                current.observed->identity ==
                    published_second.observed.identity,
            "Authoritative lookup disagreed with the published token.");
}

void test_publication_boundary_temp_same_inode_rewrite_does_not_publish_foreign_bytes() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string publication = encode_reviewed_source_state(second);
    const std::string foreign = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_C)));
    require(foreign != publication, "Decoy bytes matched A's publication.");
    RewritingTemporarySourceInPlace rewriter{foreign, {}, 0, 0};
    RewritingTemporarySourceInPlace::instance = &rewriter;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &RewritingTemporarySourceInPlace::handler);
    const auto published_second =
        require_arm<ReviewedSourceStateStorePublished>(
            publish_reviewed_source_state(second, published.observed),
            "Descriptor-bound publication failed after same-inode decoy rewrite.");
    RewritingTemporarySourceInPlace::instance = nullptr;
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        published_second.observed.leaf_name;
    require(read_bytes(successor) == publication,
            "Same-inode temp rewrite published foreign bytes.");
    require(read_bytes(successor) != foreign,
            "Published record carried the rewritten decoy bytes.");
    require(rewriter.observed_source_inode != 0,
            "Publication source inode was not exposed to the race hook.");
    const FileIdentity successor_identity = file_identity(successor);
    require(successor_identity.device == rewriter.observed_source_device &&
                successor_identity.inode == rewriter.observed_source_inode,
            "Published name did not resolve to the retained O_TMPFILE inode.");
    require(published_second.observed.identity.device ==
                    successor_identity.device &&
                published_second.observed.identity.inode ==
                    successor_identity.inode,
            "Result token identity disagreed with the on-disk successor.");
    require(file_identity(rewriter.decoy_path).inode !=
                successor_identity.inode,
            "Publication inode silently tracked the rewritten decoy.");
    require(read_bytes(rewriter.decoy_path) == foreign,
            "Rewritten decoy was consumed as the publication source.");
    const auto current = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Post same-inode temp rewrite lookup failed.");
    require(current.observed.has_value() &&
                current.observed->identity ==
                    published_second.observed.identity,
            "Authoritative lookup disagreed with the published token.");
}

void test_ancestor_generation1_contents_rewrite_is_fail_closed() {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string rewritten = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B)));
    rewrite_path_in_place(fixture.origin, rewritten, 0600);
    require_higher_generation_fail_closed(
        fixture, rewritten, fixture.origin,
        "Generation 1 contents rewrite was flattened to lower Loaded.");
}

void test_ancestor_generation1_inode_replace_is_fail_closed() {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string replacement = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B)));
    replace_path_with_new_inode(fixture.origin, replacement, 0600);
    require_higher_generation_fail_closed(
        fixture, replacement, fixture.origin,
        "Generation 1 inode replacement was flattened to lower Loaded.");
}

void test_ancestor_generation2_contents_rewrite_is_fail_closed() {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string rewritten = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A)));
    rewrite_path_in_place(fixture.generation2, rewritten, 0600);
    require_higher_generation_fail_closed(
        fixture, rewritten, fixture.generation2,
        "Generation 2 contents rewrite rewound to ordinary Loaded.");
}

void test_ancestor_generation2_inode_replace_is_fail_closed() {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string replacement = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A)));
    replace_path_with_new_inode(fixture.generation2, replacement, 0600);
    require_higher_generation_fail_closed(
        fixture, replacement, fixture.generation2,
        "Generation 2 inode replacement rewound to ordinary Loaded.");
}

// LANDMINE: this test must not treat "the ctime changed" as closure evidence.
// Linux only guarantees that userspace cannot move ctime backwards, not that
// every mutation lands on a distinct representable value, and O_TMPFILE-capable
// filesystems without FS_MGTIME can repeat a ctime inside one timestamp
// quantum. The branch below states which capability the host filesystem
// actually has. Correctness in the coarse branch comes from
// test_post_commit_ancestor_mutation_never_disowns_committed_successor: a
// restored predecessor can only re-match a successor that was committed, and a
// committed successor is never reported as an ordinary definite failure.
void test_stale_branch_restore_does_not_reactivate() {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string original = read_bytes(fixture.origin);
    const FileIdentity original_identity = file_identity(fixture.origin);
    const StatusChangeTime observed_change = status_change_time(fixture.origin);
    const std::string rewritten = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B)));
    rewrite_path_in_place(fixture.origin, rewritten, 0600);
    require(is_unsafe_history(read_reviewed_source_state(fixture.first.package_base())),
            "Ancestor rewrite did not fail closed before restore.");
    rewrite_path_in_place(fixture.origin, original, 0600);
    require(file_identity(fixture.origin).inode == original_identity.inode,
            "Restore fixture replaced the origin inode.");
    require(read_bytes(fixture.origin) == original,
            "Restore fixture did not restore the origin bytes.");
    const StatusChangeTime restored_change = status_change_time(fixture.origin);
    const auto restored = read_reviewed_source_state(fixture.first.package_base());
    if(restored_change != observed_change) {
        require(is_unsafe_history(restored),
                "Distinguishable ctime did not separate the restored record.");
    } else {
        // Coarse-timestamp filesystem: the restored record is byte-for-byte and
        // metadata-for-metadata the record generation 2 was bound to, so the
        // chain is genuinely complete again and reporting it is honest.
        const auto& read = require_arm<ReviewedSourceStateStoreRead>(
            restored, "Restored complete chain was not a store read.");
        require(read.observed.has_value() && read.observed->generation == 3,
                "Restored complete chain did not resolve to generation 3.");
    }
    require(read_bytes(fixture.generation3) ==
                encode_reviewed_source_state(fixture.third),
            "Restore mutated generation 3.");
}

// The pure-protocol form of the coarse-ctime restore: a successor whose leaf
// binds the predecessor's exact current device, inode, status-change time, and
// content digest. This is byte-for-byte the directory a same-inode X -> Y -> X
// restore leaves behind on a filesystem that cannot separate the two writes.
// The store accepts it, which is safe precisely because such a record can only
// exist if some writer committed it, and the commit taxonomy never disowns a
// committed record.
void test_matching_predecessor_binding_is_accepted_and_consistent() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    const fs::path origin = origin_path(first.package_base());
    const ReviewedSourceStateRecordIdentity live = record_identity_of(origin);
    require(live.status_change_time_seconds ==
                    published.observed.identity.status_change_time_seconds &&
                live.status_change_time_nanoseconds ==
                    published.observed.identity
                        .status_change_time_nanoseconds,
            "Origin status-change time drifted before the fixture was built.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string planted = encode_reviewed_source_state(second);
    const std::string leaf = reviewed_source_state_store_successor_leaf(
        2, live, published.observed.raw_contents);
    write_bytes(package_dir / leaf, planted, 0600);

    const auto result = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Matching predecessor binding was not a store read.");
    require(require_arm<ReviewedSourceStateLoaded>(
                result.observation, "Matching binding was not Loaded.")
                    .state == second,
            "Matching binding did not resolve to the planted successor.");
    require(result.observed.has_value() &&
                result.observed->generation == 2 &&
                result.observed->leaf_name == leaf &&
                result.observed->raw_contents == planted,
            "Accepted tip disagreed with the planted record.");
    require(read_bytes(origin) == published.observed.raw_contents,
            "Accepting the matching binding mutated the origin.");
}

void test_device_renumbering_preserves_reviewed_source_identity() {
    const std::string remote = "https://aur.archlinux.org/google-chrome.git";
    const ReviewedSourceState first = aur_state("google-chrome", remote);
    const ReviewedSourceState successors[] = {
        aur_state("google-chrome", remote, std::string(SHA1_B)),
        aur_state("other-base", remote, std::string(SHA1_B)),
        aur_state("google-chrome", "https://aur.archlinux.org/other-base.git", std::string(SHA1_B))};
    for(const ReviewedSourceState& successor : successors) {
        StoreTestHome home;
        const auto published = require_arm<ReviewedSourceStateStorePublished>(
            publish_reviewed_source_state(first, std::nullopt),
            "Device-renumber reviewed-source seed failed.");
        auto legacy_identity = published.observed.identity;
        legacy_identity.device ^= 1;
        const std::string leaf = reviewed_source_state_store_successor_leaf(
            2, legacy_identity, published.observed.raw_contents);
        const fs::path successor_path =
            reviewed_source_state_store_entry_path(first.package_base()) / leaf;
        const std::string bytes = encode_reviewed_source_state(successor);
        write_bytes(successor_path, bytes, 0600);

        const auto read = require_arm<ReviewedSourceStateStoreRead>(
            read_reviewed_source_state(first.package_base()),
            "Device-only predecessor mismatch was unsafe reviewed history.");
        require(read.observed.has_value() && read.observed->generation == 2 &&
                    read.observed->leaf_name == leaf && read.observed->raw_contents == bytes &&
                    read.observed->identity == record_identity_of(successor_path),
                "Device-renumber reviewed-source observation lost the current record.");
        if(successor.package_base() == first.package_base()) {
            require(require_arm<ReviewedSourceStateLoaded>(
                        read.observation, "Device-renumber reviewed source was not Loaded.")
                            .state == successor,
                    "Device-renumber reviewed source changed the reviewed revision.");
        } else {
            const auto& mismatch = require_arm<ReviewedSourceStateSourceMismatch>(
                read.observation, "Device renumbering promoted mismatched reviewed source to Loaded.");
            const auto expected_reason = successor.package_base().package_base() != first.package_base().package_base()
                                             ? ReviewedSourceStateMismatchReason::PackageBaseMismatch
                                             : ReviewedSourceStateMismatchReason::CanonicalGitRemoteMismatch;
            require(mismatch.reasons == std::vector{expected_reason},
                    "Device-renumber reviewed-source mismatch reason drifted.");
        }
        require(read_bytes(origin_path(first.package_base())) == published.observed.raw_contents &&
                    read_bytes(successor_path) == bytes,
                "Device-renumber reviewed-source lookup rewrote the history.");
    }
}

void test_same_generation_fork_is_fail_closed() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const auto second = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_B)),
            published.observed),
        "Matching successor publication failed.");
    ReviewedSourceStateRecordIdentity other = published.observed.identity;
    other.inode += 1;
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    const std::string fork_leaf = reviewed_source_state_store_successor_leaf(
        2, other, published.observed.raw_contents);
    write_bytes(package_dir / fork_leaf, encode_reviewed_source_state(aur_state("example-base", "https://aur.archlinux.org/example-base.git", std::string(SHA1_C))),
                0600);
    const auto result = read_reviewed_source_state(first.package_base());
    require(is_unsafe_history(result),
            "Same-generation fork was flattened to the matching Loaded branch.");
    require(require_arm<ReviewedSourceStateStoreUnsafeHistory>(
                result, "Fork was not typed.")
                    .issue == ReviewedSourceStateStoreHistoryIssue::ForkDetected,
            "Same-generation fork was not ForkDetected.");
    require(fs::exists(package_dir / second.observed.leaf_name),
            "Matching successor was removed.");
    require(fs::exists(package_dir / fork_leaf), "Fork successor was removed.");
}

void test_future_orphan_successor_is_fail_closed() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    ReviewedSourceStateRecordIdentity other = published.observed.identity;
    other.inode += 7;
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    const std::string future_leaf = reviewed_source_state_store_successor_leaf(
        2, other, published.observed.raw_contents);
    const std::string future = "schema_version = 2\nnext_field = true\n";
    write_bytes(package_dir / future_leaf, future, 0600);
    const auto result = read_reviewed_source_state(first.package_base());
    require(is_unsafe_history(result),
            "Future orphan successor was ignored so current Loaded could advance.");
    const auto& history = require_arm<ReviewedSourceStateStoreUnsafeHistory>(
        result, "Future orphan was not typed unsafe history.");
    require(history.issue ==
                    ReviewedSourceStateStoreHistoryIssue::FutureOwnedArtifact ||
                history.issue ==
                    ReviewedSourceStateStoreHistoryIssue::
                        HigherGenerationUnreachable ||
                history.issue ==
                    ReviewedSourceStateStoreHistoryIssue::OrphanSuccessor,
            "Future orphan classification drifted.");
    require(read_bytes(package_dir / future_leaf) == future,
            "Future orphan bytes were overwritten.");
    require(read_bytes(origin_path(first.package_base())) ==
                published.observed.raw_contents,
            "Future orphan mutated the current origin.");
}

void test_package_directory_replacement_before_commit_is_not_published() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const std::string marker = "p-new-untouched\n";
    ReplacingPackageDirectory replacer{marker};
    ReplacingPackageDirectory::instance = &replacer;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &ReplacingPackageDirectory::handler);
    const auto conflict = require_arm<ReviewedSourceStateStoreFailure>(
        publish_reviewed_source_state(
            aur_state(
                "example-base",
                "https://aur.archlinux.org/example-base.git",
                std::string(SHA1_B)),
            published.observed),
        "Pre-commit PackageBase replacement was treated as Published.");
    ReplacingPackageDirectory::instance = nullptr;
    require(conflict.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Pre-commit PackageBase replacement was not ConcurrentReplacement.");
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    require(read_bytes(package_dir / "-.moguet-reviewed-source-p-new-marker") ==
                marker,
            "A mutated P-new during pre-commit PackageBase replacement.");
    require(!contains_leaf(regular_leaf_names(package_dir), "1.toml"),
            "A published into named P-new before commit.");
    const auto lookup = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Named P-new lookup failed.");
    require(std::holds_alternative<ReviewedSourceStateMissing>(lookup.observation),
            "Named P-new claimed A's publication.");
}

void test_package_directory_replacement_after_commit_is_published_uncertain() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const std::string marker = "p-new-after-commit\n";
    ReplacingPackageDirectory replacer{marker};
    ReplacingPackageDirectory::instance = &replacer;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterPublication,
        &ReplacingPackageDirectory::handler);
    const auto uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            publish_reviewed_source_state(
                aur_state(
                    "example-base",
                    "https://aur.archlinux.org/example-base.git",
                    std::string(SHA1_B)),
                published.observed),
            "Post-commit PackageBase replacement was ordinary Published.");
    ReplacingPackageDirectory::instance = nullptr;
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    UnitDirectoryIdentityUncertain,
            "Post-commit PackageBase replacement issue drifted.");
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    require(read_bytes(package_dir / "-.moguet-reviewed-source-p-new-marker") ==
                marker,
            "A mutated P-new after commit.");
    require(!contains_leaf(regular_leaf_names(package_dir), "1.toml"),
            "Named P-new received A's origin after commit.");
    const auto lookup = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Named P-new lookup after commit failed.");
    require(std::holds_alternative<ReviewedSourceStateMissing>(lookup.observation),
            "Named P-new lookup claimed A's detached publication.");
}

void test_post_commit_predecessor_status_failure_is_published_uncertain() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::PostCommitPredecessorStatus);
    const auto uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            publish_reviewed_source_state(second, published.observed),
            "Post-commit predecessor status failure was flattened.");
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    PredecessorObservationUncertain,
            "Predecessor status observation failure issue drifted.");
    require(uncertain.observed.has_value(),
            "Predecessor status observation failure lost the published token.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(second.package_base()) /
        uncertain.observed->leaf_name;
    require(read_bytes(successor) == encode_reviewed_source_state(second),
            "Predecessor status observation failure rolled back the successor.");
    require(uncertain.observed->identity == record_identity_of(successor),
            "Predecessor status observation failure token drifted from disk.");
    const auto lookup = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(second.package_base()),
        "Authoritative lookup after predecessor status failure failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                lookup.observation, "Successor was not Loaded.")
                    .state == second,
            "Authoritative lookup disagreed with the uncertain publication.");
    require(lookup.observed.has_value() &&
                lookup.observed->leaf_name == uncertain.observed->leaf_name,
            "Authoritative lookup resolved a different leaf.");
}

void test_post_commit_predecessor_read_failure_is_published_uncertain() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    fail_next_reviewed_source_state_store_operation_for_test(
        ReviewedSourceStateStoreTestFailurePoint::PostCommitPredecessorRead);
    const auto uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            publish_reviewed_source_state(second, published.observed),
            "Post-commit predecessor read failure was flattened.");
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    PredecessorObservationUncertain,
            "Predecessor read observation failure issue drifted.");
    require(uncertain.system_error.has_value(),
            "Predecessor read observation failure lost the system error.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(second.package_base()) /
        uncertain.observed->leaf_name;
    require(fs::exists(successor),
            "Predecessor read observation failure removed the successor.");
    require(read_bytes(successor) == encode_reviewed_source_state(second),
            "Predecessor read observation failure mutated the successor bytes.");
    require(uncertain.observed->identity == record_identity_of(successor),
            "Predecessor read observation failure token drifted from disk.");
    const auto lookup = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(second.package_base()),
        "Authoritative lookup after predecessor read failure failed.");
    require(require_arm<ReviewedSourceStateLoaded>(
                lookup.observation, "Successor was not Loaded.")
                    .state == second,
            "Authoritative lookup disagreed with the uncertain publication.");
    require(lookup.observed.has_value() &&
                lookup.observed->leaf_name == uncertain.observed->leaf_name,
            "Authoritative lookup resolved a different leaf.");
}

// M411-S2-008A publish side. A proves 1 -> 2 -> 3 and is publishing generation
// 4. The mutation lands strictly after that proof, either before the kernel
// commit point or between the pre-commit reproof and linkat. Neither ordinary
// arm is allowed, and the result must not contradict the very next lookup.
void require_publication_ancestor_mutation_is_fail_closed(
    ReviewedSourceStateStoreTestRacePoint boundary,
    bool should_mutate_origin,
    bool should_replace_inode,
    const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const ReviewedSourceState fourth = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const std::string publication = encode_reviewed_source_state(fourth);
    const std::string successor_leaf =
        reviewed_source_state_store_successor_leaf(
            4, fixture.published_third->observed.identity,
            fixture.published_third->observed.raw_contents);
    const fs::path successor = fixture.package_dir / successor_leaf;
    const fs::path target =
        should_mutate_origin ? fixture.origin : fixture.generation2;
    const std::string mutated = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_C)));
    const std::string generation3_bytes = read_bytes(fixture.generation3);

    MutatingAncestor mutator{target, mutated, should_replace_inode};
    MutatingAncestor::instance = &mutator;
    run_reviewed_source_state_store_race_once_for_test(
        boundary, &MutatingAncestor::handler);
    const auto result = publish_reviewed_source_state(
        fourth, fixture.published_third->observed);
    MutatingAncestor::instance = nullptr;

    require(!std::holds_alternative<ReviewedSourceStateStorePublished>(result),
            label + ": ancestor mutation still returned ordinary Published.");
    if(boundary ==
       ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary) {
        require(std::holds_alternative<ReviewedSourceStateStoreFailure>(
                    result) ||
                    std::holds_alternative<
                        ReviewedSourceStateStoreUnsafeHistory>(result),
                label + ": pre-commit mutation was not a definite refusal.");
        require(!fs::exists(successor),
                label + ": pre-commit refusal linked a successor.");
    } else {
        const auto& uncertain =
            require_arm<ReviewedSourceStateStorePublishedUncertain>(
                result, label + ": post-commit mutation was not "
                                "PublishedUncertain.");
        require(uncertain.issue ==
                    ReviewedSourceStatePostPublicationIssue::
                        AuthoritativeHistoryUncertain,
                label + ": post-commit mutation issue drifted.");
        require(uncertain.observed.has_value() &&
                    uncertain.observed->leaf_name == successor_leaf,
                label + ": uncertain publication lost the committed token.");
        require(uncertain.leftover_artifact.has_value() &&
                    *uncertain.leftover_artifact == successor,
                label + ": uncertain publication hid the committed artifact.");
        require(fs::exists(successor) && read_bytes(successor) == publication,
                label + ": committed successor was rolled back.");
        require(uncertain.observed->identity == record_identity_of(successor),
                label + ": uncertain token drifted from the on-disk record.");
    }

    require(read_bytes(target) == mutated,
            label + ": mutation fixture lost the injected bytes.");
    require(read_bytes(fixture.generation3) == generation3_bytes,
            label + ": publication mutated generation 3.");
    require(fs::exists(fixture.generation2) && fs::exists(fixture.generation3),
            label + ": higher generations were removed.");

    const auto lookup = read_reviewed_source_state(fixture.first.package_base());
    require(is_unsafe_history(lookup),
            label + ": immediate lookup contradicted the publication result.");
    const auto& history = require_arm<ReviewedSourceStateStoreUnsafeHistory>(
        lookup, label + ": lookup was not typed unsafe history.");
    require(history.highest_generation.has_value() &&
                *history.highest_generation >= 3,
            label + ": lookup hid the leftover higher generations.");
}

void test_publication_proof_boundary_ancestor_mutations_are_fail_closed() {
    struct Case {
        ReviewedSourceStateStoreTestRacePoint boundary;
        bool should_mutate_origin;
        bool should_replace_inode;
        const char* label;
    };
    static constexpr ReviewedSourceStateStoreTestRacePoint pre_commit =
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary;
    static constexpr ReviewedSourceStateStoreTestRacePoint post_commit =
        ReviewedSourceStateStoreTestRacePoint::AfterAuthorityProof;
    const Case cases[] = {
        {pre_commit, true, false, "pre-commit generation 1 rewrite"},
        {pre_commit, true, true, "pre-commit generation 1 inode replace"},
        {pre_commit, false, false, "pre-commit generation 2 rewrite"},
        {pre_commit, false, true, "pre-commit generation 2 inode replace"},
        {post_commit, true, false, "post-commit generation 1 rewrite"},
        {post_commit, true, true, "post-commit generation 1 inode replace"},
        {post_commit, false, false, "post-commit generation 2 rewrite"},
        {post_commit, false, true, "post-commit generation 2 inode replace"},
    };
    for(const Case& scenario : cases) {
        require_publication_ancestor_mutation_is_fail_closed(
            scenario.boundary, scenario.should_mutate_origin,
            scenario.should_replace_inode, scenario.label);
    }
}

// M411-S2-008A lookup side. The ancestors were already read and closed by the
// chain proof, so only a boundary reproof can notice that generation 1 or 2
// moved while the tip stayed still.
void require_lookup_ancestor_mutation_is_fail_closed(
    bool should_mutate_origin,
    bool should_replace_inode,
    const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const fs::path target =
        should_mutate_origin ? fixture.origin : fixture.generation2;
    const std::string mutated = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_C)));
    const std::string generation3_bytes = read_bytes(fixture.generation3);

    MutatingAncestor mutator{target, mutated, should_replace_inode};
    MutatingAncestor::instance = &mutator;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterReadAuthorityProof,
        &MutatingAncestor::handler);
    const auto result = read_reviewed_source_state(fixture.first.package_base());
    MutatingAncestor::instance = nullptr;

    require(is_unsafe_history(result) ||
                std::holds_alternative<ReviewedSourceStateStoreFailure>(
                    result),
            label + ": stale chain proof still returned ordinary Loaded.");
    require(read_bytes(target) == mutated,
            label + ": mutation fixture lost the injected bytes.");
    require(read_bytes(fixture.generation3) == generation3_bytes,
            label + ": lookup mutated generation 3.");
}

void test_lookup_proof_boundary_ancestor_mutations_are_fail_closed() {
    require_lookup_ancestor_mutation_is_fail_closed(
        true, false, "lookup generation 1 rewrite");
    require_lookup_ancestor_mutation_is_fail_closed(
        true, true, "lookup generation 1 inode replace");
    require_lookup_ancestor_mutation_is_fail_closed(
        false, false, "lookup generation 2 rewrite");
    require_lookup_ancestor_mutation_is_fail_closed(
        false, true, "lookup generation 2 inode replace");
}

// M411-S2-008A inventory side: a fork, a future-owned successor, or an
// unrecognized managed name appearing after the inventory was proven.
enum class PlantedEntryKind {
    SameGenerationFork,
    FutureSuccessor,
    UnrecognizedManagedEntry,
};

struct PlantedEntry {
    std::string leaf;
    std::string contents;
};

PlantedEntry make_planted_entry(
    PlantedEntryKind kind, const ThreeGenerationFixture& fixture) {
    switch(kind) {
        case PlantedEntryKind::SameGenerationFork: {
            ReviewedSourceStateRecordIdentity other =
                fixture.published_second->observed.identity;
            other.inode += 1;
            return PlantedEntry{
                reviewed_source_state_store_successor_leaf(
                    3, other,
                    fixture.published_second->observed.raw_contents),
                encode_reviewed_source_state(aur_state(
                    "example-base",
                    "https://aur.archlinux.org/example-base.git",
                    std::string(SHA1_C)))};
        }
        case PlantedEntryKind::FutureSuccessor: {
            ReviewedSourceStateRecordIdentity other =
                fixture.published_third->observed.identity;
            other.inode += 7;
            return PlantedEntry{
                reviewed_source_state_store_successor_leaf(
                    4, other,
                    fixture.published_third->observed.raw_contents),
                "schema_version = 2\nnext_field = true\n"};
        }
        case PlantedEntryKind::UnrecognizedManagedEntry:
            break;
    }
    return PlantedEntry{"unrecognized-managed.toml", "unrecognized = true\n"};
}

void require_publication_inventory_addition_is_fail_closed(
    ReviewedSourceStateStoreTestRacePoint boundary,
    PlantedEntryKind kind,
    const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const PlantedEntry planted = make_planted_entry(kind, fixture);
    const ReviewedSourceState fourth = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const std::string successor_leaf =
        reviewed_source_state_store_successor_leaf(
            4, fixture.published_third->observed.identity,
            fixture.published_third->observed.raw_contents);
    const fs::path successor = fixture.package_dir / successor_leaf;
    require(planted.leaf != successor_leaf,
            label + ": planted entry collided with A's successor leaf.");

    PlantingManagedLeaf planter{planted.leaf, planted.contents};
    PlantingManagedLeaf::instance = &planter;
    run_reviewed_source_state_store_race_once_for_test(
        boundary, &PlantingManagedLeaf::handler);
    const auto result = publish_reviewed_source_state(
        fourth, fixture.published_third->observed);
    PlantingManagedLeaf::instance = nullptr;

    require(!std::holds_alternative<ReviewedSourceStateStorePublished>(result),
            label + ": inventory addition still returned ordinary Published.");
    if(boundary ==
       ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary) {
        require(!fs::exists(successor),
                label + ": pre-commit refusal linked a successor.");
    } else {
        const auto& uncertain =
            require_arm<ReviewedSourceStateStorePublishedUncertain>(
                result, label + ": post-commit addition was not "
                                "PublishedUncertain.");
        require(uncertain.issue ==
                    ReviewedSourceStatePostPublicationIssue::
                        AuthoritativeHistoryUncertain,
                label + ": post-commit addition issue drifted.");
    }
    require(read_bytes(fixture.package_dir / planted.leaf) == planted.contents,
            label + ": planted entry bytes were destroyed.");
    require(read_bytes(fixture.generation3) ==
                encode_reviewed_source_state(fixture.third),
            label + ": publication mutated generation 3.");

    const auto lookup = read_reviewed_source_state(fixture.first.package_base());
    require(is_unsafe_history(lookup),
            label + ": lookup flattened the added managed entry.");
}

void test_publication_proof_boundary_inventory_additions_are_fail_closed() {
    static constexpr ReviewedSourceStateStoreTestRacePoint pre_commit =
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary;
    static constexpr ReviewedSourceStateStoreTestRacePoint post_commit =
        ReviewedSourceStateStoreTestRacePoint::AfterAuthorityProof;
    for(const ReviewedSourceStateStoreTestRacePoint boundary :
        {pre_commit, post_commit}) {
        const std::string prefix =
            boundary == pre_commit ? "pre-commit " : "post-commit ";
        require_publication_inventory_addition_is_fail_closed(
            boundary, PlantedEntryKind::SameGenerationFork,
            prefix + "fork");
        require_publication_inventory_addition_is_fail_closed(
            boundary, PlantedEntryKind::FutureSuccessor,
            prefix + "future successor");
        require_publication_inventory_addition_is_fail_closed(
            boundary, PlantedEntryKind::UnrecognizedManagedEntry,
            prefix + "unrecognized managed entry");
    }
}

void require_lookup_inventory_addition_is_fail_closed(
    PlantedEntryKind kind, const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const PlantedEntry planted = make_planted_entry(kind, fixture);
    PlantingManagedLeaf planter{planted.leaf, planted.contents};
    PlantingManagedLeaf::instance = &planter;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterReadAuthorityProof,
        &PlantingManagedLeaf::handler);
    const auto result = read_reviewed_source_state(fixture.first.package_base());
    PlantingManagedLeaf::instance = nullptr;

    require(is_unsafe_history(result) ||
                std::holds_alternative<ReviewedSourceStateStoreFailure>(
                    result),
            label + ": added managed entry still returned ordinary Loaded.");
    require(read_bytes(fixture.package_dir / planted.leaf) == planted.contents,
            label + ": planted entry bytes were destroyed.");
}

void test_lookup_proof_boundary_inventory_additions_are_fail_closed() {
    require_lookup_inventory_addition_is_fail_closed(
        PlantedEntryKind::SameGenerationFork, "lookup fork");
    require_lookup_inventory_addition_is_fail_closed(
        PlantedEntryKind::FutureSuccessor, "lookup future successor");
    require_lookup_inventory_addition_is_fail_closed(
        PlantedEntryKind::UnrecognizedManagedEntry,
        "lookup unrecognized managed entry");
}

// M411-S2-008B. The kernel commit point is the taxonomy boundary: once the
// successor is named, the operation reports PublishedUncertain, never an
// ordinary definite failure. That is what makes a later restore of the mutated
// predecessor unable to resurrect a record the caller was told did not happen,
// without assuming anything about timestamp granularity.
void test_post_commit_ancestor_mutation_never_disowns_committed_successor() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path origin = origin_path(first.package_base());
    const std::string original = read_bytes(origin);
    const ReviewedSourceState second = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B));
    const std::string publication = encode_reviewed_source_state(second);
    const std::string mutated = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_C)));

    MutatingAncestor mutator{origin, mutated, false};
    MutatingAncestor::instance = &mutator;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterAuthorityProof,
        &MutatingAncestor::handler);
    const auto result =
        publish_reviewed_source_state(second, published.observed);
    MutatingAncestor::instance = nullptr;

    require(!std::holds_alternative<ReviewedSourceStateStoreFailure>(result),
            "A committed successor was reported as an ordinary definite failure.");
    const auto& uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            result, "Post-commit predecessor mutation was not uncertain.");
    require(uncertain.observed.has_value(),
            "Uncertain publication lost the committed token.");
    const fs::path successor =
        reviewed_source_state_store_entry_path(first.package_base()) /
        uncertain.observed->leaf_name;
    require(read_bytes(successor) == publication,
            "Committed successor bytes drifted.");
    require(is_unsafe_history(read_reviewed_source_state(first.package_base())),
            "Mutated predecessor was flattened to Loaded.");

    // B restores the predecessor. The successor may become the chain tip again;
    // that is consistent, because A never claimed the record did not exist.
    rewrite_path_in_place(origin, original, 0600);
    const auto restored = read_reviewed_source_state(first.package_base());
    if(const auto* read =
           std::get_if<ReviewedSourceStateStoreRead>(&restored)) {
        require(read->observed.has_value() &&
                    read->observed->leaf_name ==
                        uncertain.observed->leaf_name &&
                    read->observed->raw_contents == publication,
                "A revived tip did not match the record A reported as uncertain.");
    } else {
        require(is_unsafe_history(restored),
                "Restored store was neither a read nor typed unsafe history.");
    }
    require(read_bytes(successor) == publication,
            "Restore mutated the committed successor.");
}

// M411-S2-009A. The lookup holds a detached S-old / P-old pair after the named
// .../reviewed-sources/aur/ directory is replaced. Answering from it would hand
// the caller a review baseline the current named store does not have.
void test_lookup_store_directory_replacement_is_not_ordinary_loaded() {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path store = reviewed_source_state_store_directory();
    const std::string marker = "s-new-untouched\n";
    ReplacingStoreDirectory replacer{"example-base", marker, {}};
    ReplacingStoreDirectory::instance = &replacer;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterReadAuthorityProof,
        &ReplacingStoreDirectory::handler);
    const auto result = read_reviewed_source_state(first.package_base());
    ReplacingStoreDirectory::instance = nullptr;

    const auto& failure = require_arm<ReviewedSourceStateStoreFailure>(
        result, "Detached aur/ tree answered an ordinary lookup.");
    require(failure.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            "Store lineage replacement was not ConcurrentReplacement.");

    require(read_bytes(replacer.new_package_directory /
                       "-.moguet-reviewed-source-s-new-marker") == marker,
            "Lookup mutated P-new under the replacement store.");
    require(!contains_leaf(
                regular_leaf_names(replacer.new_package_directory),
                "1.toml"),
            "Lookup published the detached record into P-new.");
    const fs::path detached = fs::path(store.string() + ".detached");
    require(read_bytes(detached / "example-base" / "1.toml") ==
                published.observed.raw_contents,
            "Detached S-old lost the original record.");

    const auto next = require_arm<ReviewedSourceStateStoreRead>(
        read_reviewed_source_state(first.package_base()),
        "Lookup after store replacement failed.");
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                next.observation),
            "Subsequent lookup did not use the replacement S-new/P-new.");
}

// M411-S2-010. Generation numbers are untrusted uint64 values from leaf names.
// UINT64_MAX must not wrap an index and a large sparse value must not become an
// allocation; both are typed unsafe history.
void require_extreme_generation_is_typed_unsafe_history(
    std::uint64_t generation, const std::string& label) {
    StoreTestHome home;
    const ReviewedSourceState first = aur_state();
    const auto published = require_arm<ReviewedSourceStateStorePublished>(
        publish_reviewed_source_state(first, std::nullopt),
        "Seed publication failed.");
    const fs::path package_dir =
        reviewed_source_state_store_entry_path(first.package_base());
    const std::string leaf = reviewed_source_state_store_successor_leaf(
        generation, published.observed.identity,
        published.observed.raw_contents);
    const std::string planted = encode_reviewed_source_state(aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_B)));
    write_bytes(package_dir / leaf, planted, 0600);

    const auto lookup = read_reviewed_source_state(first.package_base());
    const auto& history = require_arm<ReviewedSourceStateStoreUnsafeHistory>(
        lookup, label + ": extreme generation was not typed unsafe history.");
    require(history.highest_generation.has_value() &&
                *history.highest_generation == generation,
            label + ": extreme generation was dropped from the report.");
    require(history.issue == ReviewedSourceStateStoreHistoryIssue::ChainGap,
            label + ": extreme generation classification drifted.");
    require(read_bytes(package_dir / leaf) == planted,
            label + ": extreme generation bytes were destroyed.");
    require(read_bytes(origin_path(first.package_base())) ==
                published.observed.raw_contents,
            label + ": extreme generation mutated the origin.");

    const auto publish_result = publish_reviewed_source_state(
        aur_state(
            "example-base",
            "https://aur.archlinux.org/example-base.git",
            std::string(SHA1_C)),
        published.observed);
    require(std::holds_alternative<ReviewedSourceStateStoreUnsafeHistory>(
                publish_result),
            label + ": publication did not fail closed on the extreme generation.");
}

void test_extreme_generations_are_typed_unsafe_history() {
    require_extreme_generation_is_typed_unsafe_history(
        std::numeric_limits<std::uint64_t>::max(), "max generation");
    require_extreme_generation_is_typed_unsafe_history(
        9007199254740993ULL, "sparse generation");
    require_extreme_generation_is_typed_unsafe_history(
        std::numeric_limits<std::uint64_t>::max() - 1, "near-max generation");
}

// M411-S2-011 shared fixture assertions. What makes the counterexample real is
// that nothing a directory rescan can see has changed: the alias lives outside
// the PackageBase directory, the record keeps its inode and its bytes, and the
// leaf inventory is untouched apart from the operation's own successor.
//
// The refusal is closed by comparing st_nlink directly. It does not depend on
// the filesystem giving the link a fresh ctime: on a coarse-granularity
// filesystem the ctime before and after the link may be represented by the same
// value, and the assertions below never mention ctime.
void require_external_alias_is_invisible_to_the_directory(
    const LinkingRecordOutsidePackage& linker,
    const fs::path& package_dir,
    const fs::path& record,
    const std::string& record_bytes,
    std::vector<std::string> expected_inventory,
    const std::string& label) {
    require(linker.did_link,
            label + ": the record-read hardlink race never fired.");
    require(linker.linked_record == record,
            label + ": the race aliased a different record.");
    require(fs::exists(linker.alias_path),
            label + ": the external alias is gone.");
    require(linker.alias_path.parent_path() != package_dir,
            label + ": the alias was placed inside the PackageBase directory, "
                    "so a directory rescan alone could have caught it.");
    const FileIdentity aliased = file_identity(linker.alias_path);
    const FileIdentity original = file_identity(record);
    require(aliased.device == original.device && aliased.inode == original.inode,
            label + ": the alias is not the same inode on the same filesystem.");
    require(link_count(record) == 2,
            label + ": the record did not end up with two links.");
    require(read_bytes(record) == record_bytes,
            label + ": the aliased record's bytes changed.");

    std::vector<std::string> observed = regular_leaf_names(package_dir);
    std::sort(observed.begin(), observed.end());
    std::sort(expected_inventory.begin(), expected_inventory.end());
    require(observed == expected_inventory,
            label + ": the PackageBase inventory changed, so this fixture no "
                    "longer isolates the single-link mutation.");
}

// M411-S2-011 read side. A is inside the final boundary reproof and has just
// read an ancestor's bytes when B gives that record a second name outside the
// PackageBase directory. Every field the proof compared before this fix - dev,
// ino, type, uid, mode, size, mtime, leaf inventory, content digest - is still
// equal, so ordinary Loaded was reachable for a state the next lookup rejects.
void require_lookup_record_read_hardlink_race_is_fail_closed(
    bool should_target_origin, const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    // Successor leaves bind the predecessor inode and ctime, so the target leaf
    // only exists once this fixture has been published.
    const std::string target_leaf =
        should_target_origin
            ? reviewed_source_state_store_origin_leaf()
            : fixture.published_second->observed.leaf_name;
    const fs::path record = fixture.package_dir / target_leaf;
    const std::string record_bytes = read_bytes(record);
    const std::string tip_bytes = read_bytes(fixture.generation3);
    const std::vector<std::string> inventory_before =
        regular_leaf_names(fixture.package_dir);
    require(link_count(record) == 1,
            label + ": the fixture record was not single-link to begin with.");

    LinkingRecordOutsidePackage linker{
        target_leaf, home.root() / "outside-alias", {}, false};
    LinkingRecordOutsidePackage::instance = &linker;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterReadAuthorityProof,
        &LinkingRecordOutsidePackage::arm);
    const auto result = read_reviewed_source_state(fixture.first.package_base());
    LinkingRecordOutsidePackage::instance = nullptr;
    reset_reviewed_source_state_store_test_hooks();

    require(!std::holds_alternative<ReviewedSourceStateStoreRead>(result),
            label + ": a record that gained a second link still produced an "
                    "ordinary read.");
    const auto& failure = require_arm<ReviewedSourceStateStoreFailure>(
        result, label + ": the refusal was not a typed failure.");
    // LANDMINE: MultipleHardLinks, not ConcurrentReplacement. The post-read
    // proof asserts the absolute security contract before it compares the
    // status fields, so the kind names the contract that was actually broken.
    // A run that reports ConcurrentReplacement here means the record was
    // refused because some other field moved - in practice ctime - and the
    // single-link proof this test exists for did not fire.
    require(failure.kind ==
                ReviewedSourceStateStoreFailureKind::MultipleHardLinks,
            label + ": the refusal did not come from the single-link proof.");

    require_external_alias_is_invisible_to_the_directory(
        linker, fixture.package_dir, record, record_bytes, inventory_before,
        label);
    require(read_bytes(fixture.generation3) == tip_bytes,
            label + ": the lookup mutated the chain tip.");

    const auto next = read_reviewed_source_state(fixture.first.package_base());
    require(!std::holds_alternative<ReviewedSourceStateStoreRead>(next),
            label + ": the immediate next lookup contradicted the refusal.");
    const auto& next_failure = require_arm<ReviewedSourceStateStoreFailure>(
        next, label + ": the subsequent lookup was not a typed failure.");
    require(next_failure.kind ==
                ReviewedSourceStateStoreFailureKind::MultipleHardLinks,
            label + ": the subsequent lookup did not report the alias.");
}

void test_lookup_record_read_hardlink_race_is_fail_closed() {
    require_lookup_record_read_hardlink_race_is_fail_closed(
        true, "lookup generation 1 external hardlink");
    require_lookup_record_read_hardlink_race_is_fail_closed(
        false, "lookup generation 2 external hardlink");
}

// M411-S2-011 publish side. A has already linked generation 4, so the record is
// permanent and no ordinary definite failure is allowed. B aliases an ancestor
// during the post-commit reproof's read of it. The answer must be a commit-aware
// PublishedUncertain, never ordinary Published and never a rollback.
void require_publication_record_read_hardlink_race_is_published_uncertain(
    bool should_target_origin, const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string target_leaf =
        should_target_origin
            ? reviewed_source_state_store_origin_leaf()
            : fixture.published_second->observed.leaf_name;
    const ReviewedSourceState fourth = aur_state(
        "example-base",
        "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const std::string publication = encode_reviewed_source_state(fourth);
    const std::string successor_leaf =
        reviewed_source_state_store_successor_leaf(
            4, fixture.published_third->observed.identity,
            fixture.published_third->observed.raw_contents);
    const fs::path successor = fixture.package_dir / successor_leaf;
    const fs::path record = fixture.package_dir / target_leaf;
    const std::string record_bytes = read_bytes(record);
    std::vector<std::string> expected_inventory =
        regular_leaf_names(fixture.package_dir);
    expected_inventory.push_back(successor_leaf);
    require(link_count(record) == 1,
            label + ": the fixture record was not single-link to begin with.");

    LinkingRecordOutsidePackage linker{
        target_leaf, home.root() / "outside-alias", {}, false};
    LinkingRecordOutsidePackage::instance = &linker;
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterAuthorityProof,
        &LinkingRecordOutsidePackage::arm);
    const auto result = publish_reviewed_source_state(
        fourth, fixture.published_third->observed);
    LinkingRecordOutsidePackage::instance = nullptr;
    reset_reviewed_source_state_store_test_hooks();

    require(!std::holds_alternative<ReviewedSourceStateStorePublished>(result),
            label + ": an ancestor that gained a second link still returned "
                    "ordinary Published.");
    const auto& uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            result, label + ": a post-commit refusal was reported as an "
                            "ordinary definite failure.");
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    AuthoritativeHistoryUncertain,
            label + ": the post-commit issue left the commit-aware taxonomy.");
    // The same discriminator as the read side: only the single-link proof
    // produces MultipleHardLinks here, so this assertion fails if the refusal
    // actually came from a ctime change rather than from the link count.
    require(uncertain.failure_kind ==
                ReviewedSourceStateStoreFailureKind::MultipleHardLinks,
            label + ": the refusal did not come from the single-link proof.");
    require(uncertain.observed.has_value() &&
                uncertain.observed->leaf_name == successor_leaf,
            label + ": the uncertain publication lost the committed token.");
    require(uncertain.leftover_artifact.has_value() &&
                *uncertain.leftover_artifact == successor,
            label + ": the uncertain publication hid the committed artifact.");
    require(fs::exists(successor) && read_bytes(successor) == publication,
            label + ": the committed successor was rolled back.");
    require(uncertain.observed->identity == record_identity_of(successor),
            label + ": the returned token drifted from the on-disk record.");

    require_external_alias_is_invisible_to_the_directory(
        linker, fixture.package_dir, record, record_bytes,
        expected_inventory, label);

    const auto next = read_reviewed_source_state(fixture.first.package_base());
    require(!std::holds_alternative<ReviewedSourceStateStoreRead>(next),
            label + ": the immediate next lookup contradicted the uncertain "
                    "publication.");
    const auto& next_failure = require_arm<ReviewedSourceStateStoreFailure>(
        next, label + ": the subsequent lookup was not a typed failure.");
    require(next_failure.kind ==
                ReviewedSourceStateStoreFailureKind::MultipleHardLinks,
            label + ": the subsequent lookup did not report the alias.");
}

void test_publication_record_read_hardlink_race_is_published_uncertain() {
    require_publication_record_read_hardlink_race_is_published_uncertain(
        true, "publish generation 1 external hardlink");
    require_publication_record_read_hardlink_race_is_published_uncertain(
        false, "publish generation 2 external hardlink");
}

// M411-S2-012 shared evidence. Unlike the single-link race above, the alias no
// longer names the current generation leaf: rename-over moves that leaf to a
// different inode and leaves both the detached opened inode and the new named
// inode at nlink == 1. The only authoritative discriminator is a post-read
// nofollow lookup of the exact leaf and a direct filesystem-identity compare.
void require_record_leaf_was_rebound(
    const RebindingRecordOutsidePackage& rebinder,
    const fs::path& package_dir,
    const fs::path& record,
    const FileIdentity& old_identity,
    const FileIdentity& replacement_identity,
    const std::string& old_bytes,
    const std::string& replacement_bytes,
    std::vector<std::string> expected_inventory,
    const std::string& label) {
    require(rebinder.did_rebind,
            label + ": the record-read leaf rebind race never fired.");
    require(rebinder.rebound_record == record,
            label + ": the race rebound a different record.");
    require(fs::exists(rebinder.alias_path),
            label + ": the detached opened inode lost its external alias.");
    require(!fs::exists(rebinder.replacement_path),
            label + ": rename-over left the replacement source name behind.");
    require(rebinder.alias_path.parent_path() != package_dir &&
                rebinder.replacement_path.parent_path() != package_dir,
            label + ": an external race artifact was placed inside the "
                    "PackageBase directory.");

    const FileIdentity alias_identity = file_identity(rebinder.alias_path);
    const FileIdentity named_identity = file_identity(record);
    require(alias_identity.device == old_identity.device &&
                alias_identity.inode == old_identity.inode,
            label + ": the external alias did not preserve the opened inode.");
    require(named_identity.device == replacement_identity.device &&
                named_identity.inode == replacement_identity.inode,
            label + ": the generation leaf did not receive the replacement inode.");
    require(alias_identity.device == named_identity.device &&
                alias_identity.inode != named_identity.inode,
            label + ": old alias and current leaf are not distinct inodes on "
                    "the same filesystem.");
    require(link_count(rebinder.alias_path) == 1,
            label + ": the detached opened inode did not return to one link.");
    require(link_count(record) == 1,
            label + ": the replacement named inode is not single-link.");
    require(read_bytes(rebinder.alias_path) == old_bytes,
            label + ": the detached opened inode bytes changed.");
    require(read_bytes(record) == replacement_bytes,
            label + ": the replacement named bytes changed.");

    std::vector<std::string> observed = regular_leaf_names(package_dir);
    std::sort(observed.begin(), observed.end());
    std::sort(expected_inventory.begin(), expected_inventory.end());
    require(observed == expected_inventory,
            label + ": the PackageBase inventory changed, so the fixture did "
                    "not isolate the leaf-to-inode association.");
}

std::string replacement_record_bytes(bool should_target_origin) {
    return encode_reviewed_source_state(aur_state(
        "example-base", "https://aur.archlinux.org/example-base.git",
        should_target_origin ? std::string(SHA1_B) : std::string(SHA1_A)));
}

void require_lookup_record_read_leaf_rebind_is_fail_closed(
    bool should_target_origin, const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string target_leaf =
        should_target_origin
            ? reviewed_source_state_store_origin_leaf()
            : fixture.published_second->observed.leaf_name;
    const fs::path record = fixture.package_dir / target_leaf;
    const std::string old_bytes = read_bytes(record);
    const std::string replacement_bytes =
        replacement_record_bytes(should_target_origin);
    require(old_bytes != replacement_bytes,
            label + ": replacement bytes did not differ from the opened record.");
    const fs::path replacement_path = home.root() / "outside-replacement";
    write_bytes(replacement_path, replacement_bytes, 0600);
    const FileIdentity old_identity = file_identity(record);
    const FileIdentity replacement_identity = file_identity(replacement_path);
    require(old_identity.device == replacement_identity.device,
            label + ": replacement was not pre-created on the same filesystem.");
    const std::string tip_bytes = read_bytes(fixture.generation3);
    const std::vector<std::string> inventory_before =
        regular_leaf_names(fixture.package_dir);

    RebindingRecordOutsidePackage rebinder{
        target_leaf, home.root() / "outside-old-alias", replacement_path, {}, false};
    RebindingRecordOutsidePackage::instance = &rebinder;
    simulate_coarse_reviewed_source_state_store_timestamps_for_test();
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterReadAuthorityProof,
        &RebindingRecordOutsidePackage::arm);
    const auto result = read_reviewed_source_state(fixture.first.package_base());
    RebindingRecordOutsidePackage::instance = nullptr;
    reset_reviewed_source_state_store_test_hooks();

    const auto& failure = require_arm<ReviewedSourceStateStoreFailure>(
        result, label + ": rebound leaf still produced ordinary Loaded.");
    require(failure.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            label + ": leaf-to-inode mismatch was not ConcurrentReplacement.");
    require_record_leaf_was_rebound(
        rebinder, fixture.package_dir, record, old_identity,
        replacement_identity, old_bytes, replacement_bytes,
        inventory_before, label);
    require(read_bytes(fixture.generation3) == tip_bytes,
            label + ": lookup mutated the chain tip.");
    require(is_unsafe_history(
                read_reviewed_source_state(fixture.first.package_base())),
            label + ": subsequent lookup contradicted the boundary refusal.");
}

void test_lookup_record_read_leaf_rebind_is_fail_closed() {
    require_lookup_record_read_leaf_rebind_is_fail_closed(
        true, "lookup generation 1 external leaf rebind");
    require_lookup_record_read_leaf_rebind_is_fail_closed(
        false, "lookup generation 2 external leaf rebind");
}

void require_pre_commit_record_read_leaf_rebind_stops_publication(
    bool should_target_origin, const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string target_leaf =
        should_target_origin
            ? reviewed_source_state_store_origin_leaf()
            : fixture.published_second->observed.leaf_name;
    const fs::path record = fixture.package_dir / target_leaf;
    const std::string old_bytes = read_bytes(record);
    const std::string replacement_bytes =
        replacement_record_bytes(should_target_origin);
    const fs::path replacement_path = home.root() / "outside-replacement";
    write_bytes(replacement_path, replacement_bytes, 0600);
    const FileIdentity old_identity = file_identity(record);
    const FileIdentity replacement_identity = file_identity(replacement_path);
    require(old_identity.device == replacement_identity.device,
            label + ": replacement was not pre-created on the same filesystem.");
    const ReviewedSourceState fourth = aur_state(
        "example-base", "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const std::string successor_leaf =
        reviewed_source_state_store_successor_leaf(
            4, fixture.published_third->observed.identity,
            fixture.published_third->observed.raw_contents);
    const fs::path successor = fixture.package_dir / successor_leaf;
    const std::vector<std::string> inventory_before =
        regular_leaf_names(fixture.package_dir);

    RebindingRecordOutsidePackage rebinder{
        target_leaf, home.root() / "outside-old-alias", replacement_path, {}, false};
    RebindingRecordOutsidePackage::instance = &rebinder;
    simulate_coarse_reviewed_source_state_store_timestamps_for_test();
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AtPublicationBoundary,
        &RebindingRecordOutsidePackage::arm);
    const auto result = publish_reviewed_source_state(
        fourth, fixture.published_third->observed);
    RebindingRecordOutsidePackage::instance = nullptr;
    reset_reviewed_source_state_store_test_hooks();

    const auto& failure = require_arm<ReviewedSourceStateStoreFailure>(
        result, label + ": pre-commit leaf rebind was not a definite refusal.");
    require(failure.kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            label + ": pre-commit mismatch was not ConcurrentReplacement.");
    require(!fs::exists(successor),
            label + ": pre-commit refusal still linked generation 4.");
    require_record_leaf_was_rebound(
        rebinder, fixture.package_dir, record, old_identity,
        replacement_identity, old_bytes, replacement_bytes,
        inventory_before, label);
    require(is_unsafe_history(
                read_reviewed_source_state(fixture.first.package_base())),
            label + ": subsequent lookup contradicted the pre-commit refusal.");
}

void test_pre_commit_record_read_leaf_rebind_stops_publication() {
    require_pre_commit_record_read_leaf_rebind_stops_publication(
        true, "pre-commit generation 1 external leaf rebind");
    require_pre_commit_record_read_leaf_rebind_stops_publication(
        false, "pre-commit generation 2 external leaf rebind");
}

void require_post_commit_record_read_leaf_rebind_is_published_uncertain(
    bool should_target_origin,
    bool should_target_current,
    const std::string& label) {
    StoreTestHome home;
    ThreeGenerationFixture fixture = publish_three_generations();
    const std::string target_leaf =
        should_target_current
            ? fixture.published_third->observed.leaf_name
        : should_target_origin
            ? reviewed_source_state_store_origin_leaf()
            : fixture.published_second->observed.leaf_name;
    const fs::path record = fixture.package_dir / target_leaf;
    const std::string old_bytes = read_bytes(record);
    const std::string replacement_bytes =
        should_target_current
            ? encode_reviewed_source_state(aur_state(
                  "example-base",
                  "https://aur.archlinux.org/example-base.git",
                  std::string(SHA1_A)))
            : replacement_record_bytes(should_target_origin);
    const fs::path replacement_path = home.root() / "outside-replacement";
    write_bytes(replacement_path, replacement_bytes, 0600);
    const FileIdentity old_identity = file_identity(record);
    const FileIdentity replacement_identity = file_identity(replacement_path);
    require(old_identity.device == replacement_identity.device,
            label + ": replacement was not pre-created on the same filesystem.");
    const ReviewedSourceState fourth = aur_state(
        "example-base", "https://aur.archlinux.org/example-base.git",
        std::string(SHA1_A));
    const std::string publication = encode_reviewed_source_state(fourth);
    const std::string successor_leaf =
        reviewed_source_state_store_successor_leaf(
            4, fixture.published_third->observed.identity,
            fixture.published_third->observed.raw_contents);
    const fs::path successor = fixture.package_dir / successor_leaf;
    std::vector<std::string> expected_inventory =
        regular_leaf_names(fixture.package_dir);
    expected_inventory.push_back(successor_leaf);

    RebindingRecordOutsidePackage rebinder{
        target_leaf, home.root() / "outside-old-alias", replacement_path, {}, false};
    RebindingRecordOutsidePackage::instance = &rebinder;
    simulate_coarse_reviewed_source_state_store_timestamps_for_test();
    run_reviewed_source_state_store_race_once_for_test(
        ReviewedSourceStateStoreTestRacePoint::AfterAuthorityProof,
        &RebindingRecordOutsidePackage::arm);
    const auto result = publish_reviewed_source_state(
        fourth, fixture.published_third->observed);
    RebindingRecordOutsidePackage::instance = nullptr;
    reset_reviewed_source_state_store_test_hooks();

    const auto& uncertain =
        require_arm<ReviewedSourceStateStorePublishedUncertain>(
            result, label + ": post-commit leaf rebind returned ordinary "
                            "Published or a definite failure.");
    require(uncertain.issue ==
                ReviewedSourceStatePostPublicationIssue::
                    AuthoritativeHistoryUncertain,
            label + ": post-commit issue left the authority taxonomy.");
    require(uncertain.failure_kind ==
                ReviewedSourceStateStoreFailureKind::ConcurrentReplacement,
            label + ": post-commit mismatch was not ConcurrentReplacement.");
    require(uncertain.observed.has_value() &&
                uncertain.observed->leaf_name == successor_leaf,
            label + ": uncertain publication lost the committed token.");
    require(uncertain.leftover_artifact.has_value() &&
                *uncertain.leftover_artifact == successor,
            label + ": uncertain publication hid the committed artifact.");
    require(fs::exists(successor) && read_bytes(successor) == publication,
            label + ": post-commit refusal rolled back generation 4.");
    require(uncertain.observed->identity == record_identity_of(successor),
            label + ": committed token drifted from the on-disk successor.");
    require_record_leaf_was_rebound(
        rebinder, fixture.package_dir, record, old_identity,
        replacement_identity, old_bytes, replacement_bytes,
        expected_inventory, label);
    require(is_unsafe_history(
                read_reviewed_source_state(fixture.first.package_base())),
            label + ": subsequent lookup contradicted PublishedUncertain.");
}

void test_post_commit_record_read_leaf_rebind_is_published_uncertain() {
    require_post_commit_record_read_leaf_rebind_is_published_uncertain(
        true, false, "post-commit generation 1 external leaf rebind");
    require_post_commit_record_read_leaf_rebind_is_published_uncertain(
        false, false, "post-commit generation 2 external leaf rebind");
    require_post_commit_record_read_leaf_rebind_is_published_uncertain(
        false, true,
        "post-commit current predecessor external leaf rebind");
}
#endif


void block_reviewed_allocations(const ReviewedSourceStateStoreTestRaceContext&) {
    allocation_fault::blocked = true;
}

void test_reviewed_resource_outcomes() {
    for(int mode : {0, 1, 2}) {
        StoreTestHome home;
        const auto state = aur_state();
        std::optional<ReviewedSourceStateStorePublishResult> result;
        allocation_fault::failures = 0;
        allocation_fault::blocked = mode == 0;
        if(mode != 0) {
            run_reviewed_source_state_store_race_once_for_test(
                mode == 1 ? ReviewedSourceStateStoreTestRacePoint::BeforePostCommitReproof
                          : ReviewedSourceStateStoreTestRacePoint::AfterVerifiedPublication,
                &block_reviewed_allocations);
        }
        try {
            result.emplace(publish_reviewed_source_state(state, std::nullopt));
        } catch(...) {
            allocation_fault::blocked = false;
            throw;
        }
        const bool stayed_blocked = allocation_fault::blocked;
        allocation_fault::blocked = false;
        require(stayed_blocked, "Reviewed allocation guard was unexpectedly reset.");
        if(mode == 0) {
            require(require_arm<ReviewedSourceStateStoreFailure>(*result, "Reviewed precommit resource failure escaped.").kind ==
                        ReviewedSourceStateStoreFailureKind::ResourceFailure,
                    "Reviewed resource cause changed.");
            require(!fs::exists(reviewed_source_state_store_directory()), "Reviewed entry failure created namespace.");
        } else if(mode == 1) {
            const auto& uncertain = require_arm<ReviewedSourceStateStorePublishedUncertain>(*result, "Reviewed committed failure was definite.");
            require(uncertain.state == state && uncertain.failure_kind == ReviewedSourceStateStoreFailureKind::ResourceFailure &&
                        uncertain.observed.has_value(),
                    "Reviewed uncertain semantic/record evidence was lost.");
        } else {
            require(require_arm<ReviewedSourceStateStorePublished>(*result, "Reviewed wrapper lost verified success.").state == state &&
                        allocation_fault::failures == 0,
                    "Reviewed wrapper allocated after success.");
        }
        if(mode != 2) require(allocation_fault::failures == 1, "Reviewed resource result allocated again.");
    }
    std::cout << "reviewed semantic resource outcome matrix PASS\n";
}

} // namespace

int main() {
    try {
        test_reviewed_resource_outcomes();
        test_missing_lookup_does_not_create();
        test_first_create_is_0600_and_round_trips();
        test_successor_leaf_binds_inode_and_content_digest();
        test_cas_replacement_and_stale_writer();
        test_missing_create_is_no_replace();
        test_raw_byte_guard_is_not_reencoded();
        test_future_schema_is_not_overwritten();
        test_source_mismatch_is_not_loaded();
        test_invalid_and_corrupted_are_not_missing();
        test_symlink_hardlink_and_mode_are_failures();
        test_oversized_record_is_typed_failure();
#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        test_fsync_and_rename_failures_preserve_destination();
        test_directory_fsync_failure_is_published_uncertain();
        test_post_commit_verify_failure_is_published_uncertain();
        test_publication_boundary_occupancy_preserves_replacement();
        test_publication_boundary_inode_replacement_preserves_b();
        test_future_schema_boundary_preserves_future_bytes();
        test_cleanup_replacement_is_not_deleted();
        test_missing_missing_boundary_preserves_first_writer();
        test_loaded_loaded_boundary_preserves_newer_writer();
        test_publication_boundary_same_inode_rewrite_preserves_b();
        test_publication_boundary_same_inode_future_rewrite_is_not_downgraded();
        test_internal_temp_is_not_authoritative();
        test_publication_boundary_temp_replacement_does_not_publish_foreign_bytes();
        test_publication_boundary_temp_same_inode_rewrite_does_not_publish_foreign_bytes();
        test_ancestor_generation1_contents_rewrite_is_fail_closed();
        test_ancestor_generation1_inode_replace_is_fail_closed();
        test_ancestor_generation2_contents_rewrite_is_fail_closed();
        test_ancestor_generation2_inode_replace_is_fail_closed();
        test_stale_branch_restore_does_not_reactivate();
        test_matching_predecessor_binding_is_accepted_and_consistent();
        test_device_renumbering_preserves_reviewed_source_identity();
        test_same_generation_fork_is_fail_closed();
        test_future_orphan_successor_is_fail_closed();
        test_package_directory_replacement_before_commit_is_not_published();
        test_package_directory_replacement_after_commit_is_published_uncertain();
        test_post_commit_predecessor_status_failure_is_published_uncertain();
        test_post_commit_predecessor_read_failure_is_published_uncertain();
        test_publication_proof_boundary_ancestor_mutations_are_fail_closed();
        test_lookup_proof_boundary_ancestor_mutations_are_fail_closed();
        test_publication_proof_boundary_inventory_additions_are_fail_closed();
        test_lookup_proof_boundary_inventory_additions_are_fail_closed();
        test_post_commit_ancestor_mutation_never_disowns_committed_successor();
        test_lookup_store_directory_replacement_is_not_ordinary_loaded();
        test_extreme_generations_are_typed_unsafe_history();
        test_lookup_record_read_hardlink_race_is_fail_closed();
        test_publication_record_read_hardlink_race_is_published_uncertain();
        test_lookup_record_read_leaf_rebind_is_fail_closed();
        test_pre_commit_record_read_leaf_rebind_stops_publication();
        test_post_commit_record_read_leaf_rebind_is_published_uncertain();
#endif
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "reviewed source state store tests: all checks passed\n";
    return 0;
}
