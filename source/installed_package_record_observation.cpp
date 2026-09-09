#include "installed_package_record_observation.hpp"

#include "package_identifier.hpp"
#include "source_artifact_install_trusted_protocol.hpp"
#include "xdg_generation_store.hpp"

#include <alpm.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Issue = InstalledRecordObservationIssue;
constexpr std::size_t MAX_PROTOCOL_BYTES = 128U * 1024U;
constexpr std::size_t MAX_LOCAL_ENTRIES = 65536;

#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
InstalledRecordObservationTestHooks g_test_hooks;
void event(InstalledRecordObservationTestEvent phase) {
    if(g_test_hooks.event) g_test_hooks.event(phase);
}
#endif

class Descriptor final {
public:
    explicit Descriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {
    }
    Descriptor& operator=(Descriptor&& other) noexcept {
        if(this != &other) {
            if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    ~Descriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }
    int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

std::string hex_bytes(std::string_view bytes) {
    constexpr char DIGITS[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for(unsigned char byte : bytes) {
        result.push_back(DIGITS[byte >> 4]);
        result.push_back(DIGITS[byte & 15]);
    }
    return result;
}

std::string decode_hex(std::string_view bytes) {
    if(bytes.size() % 2 != 0 || bytes.size() > MAX_PROTOCOL_BYTES) throw Issue::MalformedMetadata;
    const auto nibble = [](char value) -> unsigned {
        if(value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if(value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
        throw Issue::MalformedMetadata;
    };
    std::string result;
    result.reserve(bytes.size() / 2);
    for(std::size_t offset = 0; offset < bytes.size(); offset += 2)
        result.push_back(static_cast<char>((nibble(bytes[offset]) << 4) | nibble(bytes[offset + 1])));
    return result;
}

std::string hex_integer(std::uint64_t value, std::size_t digits) {
    std::string result(digits, '0');
    constexpr char DIGITS[] = "0123456789abcdef";
    for(std::size_t index = 0; index < digits; ++index) {
        result[digits - index - 1] = DIGITS[value & 15];
        value >>= 4;
    }
    return result;
}

bool field_valid(std::string_view value) {
    return !value.empty() && value.size() <= 4096 && std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte > 0x20 && byte != 0x7f && byte != '/';
    });
}

struct Identity {
    std::uint64_t device, inode, mode, owner, group, links, size;
    std::int64_t mtime_seconds, mtime_nanoseconds, ctime_seconds, ctime_nanoseconds;
    bool operator==(const Identity&) const = default;

    static Identity from_stat(const struct stat& metadata, bool contents) {
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        if(g_test_hooks.ignore_directory_content_changes && S_ISDIR(metadata.st_mode)) contents = false;
#endif
        return {static_cast<std::uint64_t>(metadata.st_dev), static_cast<std::uint64_t>(metadata.st_ino),
                static_cast<std::uint64_t>(metadata.st_mode), static_cast<std::uint64_t>(metadata.st_uid),
                static_cast<std::uint64_t>(metadata.st_gid), contents ? static_cast<std::uint64_t>(metadata.st_nlink) : 0,
                contents ? static_cast<std::uint64_t>(metadata.st_size) : 0,
                contents ? metadata.st_mtim.tv_sec : 0, contents ? metadata.st_mtim.tv_nsec : 0,
                contents ? metadata.st_ctim.tv_sec : 0, contents ? metadata.st_ctim.tv_nsec : 0};
    }
    std::string encode() const {
        return std::to_string(device) + ":" + std::to_string(inode) + ":" + std::to_string(mode) + ":" +
               std::to_string(owner) + ":" + std::to_string(group) + ":" + std::to_string(links) + ":" +
               std::to_string(size) + ":" + std::to_string(mtime_seconds) + ":" + std::to_string(mtime_nanoseconds) +
               ":" + std::to_string(ctime_seconds) + ":" + std::to_string(ctime_nanoseconds);
    }
};

uid_t expected_owner() {
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    if(g_test_hooks.database_path) return g_test_hooks.expected_owner;
#endif
    return 0;
}

struct RetainedEntry {
    Descriptor descriptor;
    int parent;
    std::string name;
    Identity identity;
    bool contents;
    void reprove() const {
        struct stat retained{}, named{};
        if(fstat(descriptor.get(), &retained) != 0 ||
           fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
           Identity::from_stat(retained, contents) != identity || Identity::from_stat(named, contents) != identity)
            throw Issue::RecordChanged;
    }
};

RetainedEntry open_entry(int parent, std::string name, bool directory, bool contents,
                         bool ancestor = false, bool executable = false) {
    Descriptor descriptor(openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (directory ? O_DIRECTORY : 0)));
    if(descriptor.get() < 0) throw Issue::UnsafeRecord;
    struct stat metadata{};
    if(fstat(descriptor.get(), &metadata) != 0) throw Issue::ReadFailure;
    bool owner_ok = metadata.st_uid == (ancestor ? 0 : expected_owner());
    bool mode_ok = (metadata.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) == 0;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    // Only fixture ancestry may pass through /tmp; production never relaxes
    // owner/writability checks for any caller-selected database path.
    if(g_test_hooks.database_path && ancestor) {
        owner_ok = metadata.st_uid == 0 || metadata.st_uid == g_test_hooks.expected_owner;
        if(name == "tmp" && (metadata.st_mode & S_ISVTX) != 0 && metadata.st_uid == 0) mode_ok = true;
    }
#endif
    if(!owner_ok || !mode_ok || metadata.st_size < 0 || metadata.st_nlink == 0 ||
       (directory ? !S_ISDIR(metadata.st_mode) : (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1)) ||
       (executable && (metadata.st_mode & S_IXUSR) == 0)) throw Issue::UnsafeRecord;
    RetainedEntry result{std::move(descriptor), parent, std::move(name), Identity::from_stat(metadata, contents), contents};
    result.reprove();
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    if(!directory && g_test_hooks.child_opened) g_test_hooks.child_opened(result.name, result.descriptor.get());
#endif
    return result;
}

class DatabaseDescriptors final {
public:
    explicit DatabaseDescriptors(const std::string& database_path) {
        if(database_path.empty() || database_path.front() != '/' || database_path.size() > 4096 ||
           database_path.find('\0') != std::string::npos || database_path.back() == '/') throw Issue::UnsupportedDatabaseWorld;
        entries_.push_back(open_entry(AT_FDCWD, "/", true, false, true));
        std::size_t offset = 1;
        while(offset < database_path.size()) {
            auto end = database_path.find('/', offset);
            if(end == std::string::npos) end = database_path.size();
            auto name = database_path.substr(offset, end - offset);
            if(name.empty() || name == "." || name == "..") throw Issue::UnsupportedDatabaseWorld;
            const int parent = entries_.back().descriptor.get();
            entries_.push_back(open_entry(parent, std::move(name), true, false, end != database_path.size()));
            offset = end + 1;
            if(entries_.size() > 128) throw Issue::UnsupportedDatabaseWorld;
        }
        const int parent = entries_.back().descriptor.get();
        entries_.push_back(open_entry(parent, "local", true, false));
        reprove();
    }
    int local() const noexcept {
        return entries_.back().descriptor.get();
    }
    std::string identity() const {
        std::string result;
        for(const auto& entry : entries_)
            result += entry.identity.encode() + ";";
        return result;
    }
    void reprove() const {
        for(const auto& entry : entries_)
            entry.reprove();
    }

private:
    std::vector<RetainedEntry> entries_;
};

struct statfs filesystem_metadata(int descriptor) {
    struct statfs metadata{};
    int result;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    if(g_test_hooks.statfs)
        result = g_test_hooks.statfs(descriptor, &metadata);
    else
#endif
        result = fstatfs(descriptor, &metadata);
    if(result != 0) throw Issue::UnsupportedGeneration;
    return metadata;
}

std::string filesystem_identity(const struct statfs& metadata) {
    // Encode the two numeric fsid words in fixed order, never host memory byte
    // order, a shell display string, or a mount namespace's transient mount ID.
    static_assert(sizeof(metadata.f_fsid.__val[0]) == sizeof(std::uint32_t));
    static_assert(sizeof(metadata.f_fsid.__val) == 2 * sizeof(std::uint32_t));
    return "fs=" + hex_integer(static_cast<std::uint64_t>(metadata.f_type), 16) + "|fsid=" +
           hex_integer(static_cast<std::uint32_t>(metadata.f_fsid.__val[0]), 8) +
           hex_integer(static_cast<std::uint32_t>(metadata.f_fsid.__val[1]), 8);
}

int get_handle(int descriptor, struct file_handle* handle, int* mount_id) {
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    if(g_test_hooks.name_to_handle) return g_test_hooks.name_to_handle(descriptor, "", handle, mount_id, AT_EMPTY_PATH);
#endif
    return name_to_handle_at(descriptor, "", handle, mount_id, AT_EMPTY_PATH);
}

std::string record_generation(int descriptor) {
    const auto filesystem = filesystem_identity(filesystem_metadata(descriptor));
    struct file_handle sizing{};
    sizing.handle_bytes = 0;
    int transient_mount_id = 0;
    errno = 0;
    if(get_handle(descriptor, &sizing, &transient_mount_id) != -1 || errno != EOVERFLOW ||
       sizing.handle_bytes == 0 || sizing.handle_bytes > INSTALLED_RECORD_MAXIMUM_HANDLE_BYTES)
        throw Issue::UnsupportedGeneration;
    const unsigned requested = sizing.handle_bytes;
    // calloc provides aligned raw storage and starts the implicit-lifetime C
    // object without overlaying a live array of a different C++ element type.
    // The flexible tail is bounded by the sizing call; no path is reopened.
    const auto byte_count = sizeof(struct file_handle) + requested;
    std::unique_ptr<struct file_handle, decltype(&std::free)> handle(
        static_cast<struct file_handle*>(std::calloc(1, byte_count)), &std::free);
    if(!handle) throw std::bad_alloc();
    handle->handle_bytes = requested;
    if(get_handle(descriptor, handle.get(), &transient_mount_id) != 0 || handle->handle_bytes != requested ||
       handle->handle_type <= 0) throw Issue::UnsupportedGeneration;
    std::string opaque;
    opaque.reserve(requested);
    for(unsigned index = 0; index < requested; ++index)
        opaque.push_back(static_cast<char>(handle->f_handle[index]));
    if(filesystem_identity(filesystem_metadata(descriptor)) != filesystem) throw Issue::GenerationMismatch;
    return "linux-name-to-handle-at-v1|" + filesystem + "|type=" +
           hex_integer(static_cast<std::uint32_t>(handle->handle_type), 8) + "|length=" +
           std::to_string(requested) + "|handle=" + hex_bytes(opaque);
}

std::string read_raw(const RetainedEntry& entry, std::size_t limit, bool text) {
    if(entry.identity.size > limit) throw Issue::MetadataTooLarge;
    std::string result(static_cast<std::size_t>(entry.identity.size), '\0');
    std::size_t offset = 0;
    while(offset < result.size()) {
        ssize_t count;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        if(g_test_hooks.pread)
            count = g_test_hooks.pread(entry.descriptor.get(), result.data() + offset,
                                       result.size() - offset, static_cast<off_t>(offset));
        else
#endif
            count = pread(entry.descriptor.get(), result.data() + offset, result.size() - offset, static_cast<off_t>(offset));
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0 || static_cast<std::size_t>(count) > result.size() - offset) throw Issue::ReadFailure;
        offset += static_cast<std::size_t>(count);
    }
    entry.reprove();
    if(text && result.find('\0') != std::string::npos) throw Issue::MalformedMetadata;
    return result;
}

std::array<std::string, 4> desc_core(std::string_view raw) {
    if(raw.empty() || raw.back() != '\n' || raw.find('\0') != std::string_view::npos) throw Issue::MalformedMetadata;
    constexpr std::array<std::string_view, 4> KEYS = {"%NAME%", "%BASE%", "%VERSION%", "%ARCH%"};
    std::array<std::string, 4> result;
    std::vector<std::string_view> seen;
    while(!raw.empty()) {
        const auto end = raw.find('\n');
        if(end == std::string_view::npos) throw Issue::MalformedMetadata;
        const auto key = raw.substr(0, end);
        raw.remove_prefix(end + 1);
        if(key.empty()) continue;
        if(key.size() < 3 || key.front() != '%' || key.back() != '%' ||
           std::find(seen.begin(), seen.end(), key) != seen.end()) throw Issue::MalformedMetadata;
        seen.push_back(key);
        const auto found = std::find(KEYS.begin(), KEYS.end(), key);
        std::string_view value;
        std::size_t values = 0;
        // The header newline has already been consumed. An empty first line
        // now terminates an empty noncore section; it must not swallow the
        // following header while searching for another pair of newlines.
        while(true) {
            const auto value_end = raw.find('\n');
            if(value_end == std::string_view::npos) throw Issue::MalformedMetadata;
            const auto line = raw.substr(0, value_end);
            raw.remove_prefix(value_end + 1);
            if(line.empty()) break;
            value = line;
            ++values;
            // XDATA entries are key=value in the public package metadata
            // contract. Without '=', libalpm's lazy DESC load fails after it
            // may already have populated all four core getters.
            if(key == "%XDATA%" && line.find('=') == std::string_view::npos) throw Issue::MalformedMetadata;
        }
        if(found != KEYS.end()) {
            if(values != 1 || !field_valid(value)) throw Issue::MalformedMetadata;
            result[static_cast<std::size_t>(found - KEYS.begin())] = std::string(value);
        }
    }
    for(const auto& value : result)
        if(value.empty()) throw Issue::MalformedMetadata;
    if(!is_valid_package_name(result[0]) || !is_valid_package_name(result[1])) throw Issue::MalformedMetadata;
    return result;
}

std::vector<std::string> relevant_entries(int local, const std::string& name) {
    const int duplicate = openat(local, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if(duplicate < 0) throw Issue::DatabaseLoadFailure;
    DIR* raw = fdopendir(duplicate);
    if(!raw) {
        static_cast<void>(close(duplicate));
        throw Issue::DatabaseLoadFailure;
    }
    const auto close_directory = [](DIR* directory) { static_cast<void>(closedir(directory)); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(raw, close_directory);
    std::vector<std::string> result;
    std::size_t count = 0;
    while(true) {
        errno = 0;
        const auto* entry = readdir(directory.get());
        if(!entry) {
            if(errno != 0) throw Issue::DatabaseLoadFailure;
            break;
        }
        if(++count > MAX_LOCAL_ENTRIES) throw Issue::MetadataTooLarge;
        const std::string leaf(entry->d_name);
        if(leaf.starts_with(name + "-")) result.push_back(leaf);
    }
    std::sort(result.begin(), result.end());
    return result;
}

struct AlpmDeleter {
    void operator()(alpm_handle_t* handle) const noexcept {
        if(handle) static_cast<void>(alpm_release(handle));
    }
};

void observe_alpm_error(void* context, alpm_loglevel_t level, const char*, va_list) noexcept {
    if((level & ALPM_LOG_ERROR) != 0) *static_cast<bool*>(context) = true;
}

InstalledPackageRecordObservation observe_record(const InstalledDatabaseWorld& world, const std::string& name) {
    if(world.root_directory != "/" || !is_valid_package_name(name)) throw Issue::UnsupportedDatabaseWorld;
    DatabaseDescriptors database(world.database_path);
    if(database.identity() != world.descriptor_identity) throw Issue::DatabaseWorldMismatch;
    const auto entries_before = relevant_entries(database.local(), name);
    // A relevant malformed/duplicate directory must not become KnownAbsent
    // just because ALPM skipped it while loading its cache.
    std::optional<std::string> record_name;
    std::optional<RetainedEntry> selected_record;
    std::optional<RetainedEntry> selected_desc;
    std::optional<RetainedEntry> selected_files;
    std::optional<RetainedEntry> selected_mtree;
    std::array<std::string, 4> admitted_core;
    std::string admitted_desc;
    for(const auto& leaf : entries_before) {
        auto candidate = open_entry(database.local(), leaf, true, true);
        auto desc = open_entry(candidate.descriptor.get(), "desc", false, true);
        // Capture every required child before desc can select this record.
        // Later child replacement must disagree with its retained descriptor,
        // even if the directory generation and all directory stat values agree.
        auto files = open_entry(candidate.descriptor.get(), "files", false, true);
        auto mtree = open_entry(candidate.descriptor.get(), "mtree", false, true);
        auto raw_desc = read_raw(desc, INSTALLED_RECORD_MAXIMUM_METADATA_BYTES, true);
        const auto core = desc_core(raw_desc);
        candidate.reprove();
        if(leaf != core[0] + "-" + core[2]) throw Issue::MalformedMetadata;
        if(core[0] != name) continue;
        if(record_name) throw Issue::MalformedMetadata;
        record_name = leaf;
        selected_files.emplace(std::move(files));
        selected_mtree.emplace(std::move(mtree));
        selected_record.emplace(std::move(candidate));
        selected_desc.emplace(std::move(desc));
        admitted_core = core;
        admitted_desc = std::move(raw_desc);
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        event(InstalledRecordObservationTestEvent::AfterRawDescAdmission);
#endif
    }
    // Both files consumed by ALPM are bounded/admitted before lazy metadata
    // loading. Keep raw bytes independent of ALPM's semantic/cache projection.
    std::string admitted_files;
    if(selected_record) {
        selected_desc->reprove();
        selected_files->reprove();
        selected_mtree->reprove();
        selected_record->reprove();
        admitted_files = read_raw(*selected_files, INSTALLED_RECORD_MAXIMUM_METADATA_BYTES, true);
    }
    database.reprove();
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::BeforeSessionOpen);
    if(g_test_hooks.fail_database_load) throw Issue::DatabaseLoadFailure;
#endif
    // This handle/cache is created here, after raw admission. In particular its
    // FILES cache has never been loaded by preparation or a caller's session.
    bool alpm_read_error = false;
    alpm_errno_t error = ALPM_ERR_OK;
    std::unique_ptr<alpm_handle_t, AlpmDeleter> handle(alpm_initialize("/", world.database_path.c_str(), &error));
    if(!handle || error != ALPM_ERR_OK || alpm_option_set_logcb(handle.get(), observe_alpm_error, &alpm_read_error) != 0)
        throw Issue::DatabaseLoadFailure;
    auto* local = alpm_get_localdb(handle.get());
    if(!local || alpm_db_get_valid(local) != 0) throw Issue::DatabaseLoadFailure;
    static_cast<void>(alpm_db_get_pkgcache(local));
    if(alpm_read_error || alpm_errno(handle.get()) != ALPM_ERR_OK) throw Issue::DatabaseLoadFailure;
    auto* package = alpm_db_get_pkg(local, name.c_str());
    if(!package && alpm_errno(handle.get()) != ALPM_ERR_PKG_NOT_FOUND) throw Issue::DatabaseLoadFailure;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::AfterSessionOpen);
#endif
    if(!package) {
        if(record_name) throw Issue::DatabaseLoadFailure;
        database.reprove();
        if(entries_before != relevant_entries(database.local(), name)) throw Issue::RecordChanged;
        return InstalledPackageRecordAbsent{};
    }
    if(!record_name) throw Issue::MetadataMismatch;
    std::array<std::string, 4> semantic;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::BeforeSemanticLoad);
#endif
    const std::array getters{alpm_pkg_get_name, alpm_pkg_get_base, alpm_pkg_get_version, alpm_pkg_get_arch};
    for(std::size_t index = 0; index < getters.size(); ++index) {
        const char* value = getters[index](package);
        if(alpm_read_error || alpm_errno(handle.get()) != ALPM_ERR_OK) throw Issue::DatabaseLoadFailure;
        if(!value || !field_valid(value)) throw Issue::MalformedMetadata;
        semantic[index] = value;
    }
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::AfterSemanticLoad);
#endif
    // libalpm local DESC errors can leave every core getter populated and do
    // not always set errno/log an error. Such an error prevents a *new* FILES
    // cache from loading. A nonempty public file list is therefore a separate
    // completion observation, never a source of our raw digest or generation.
    // An empty list cannot distinguish failure from a fileless package: no
    // binding proof in that case. This never changes the transaction outcome.
    const auto* loaded_files = alpm_pkg_get_files(package);
    if(alpm_read_error || alpm_errno(handle.get()) != ALPM_ERR_OK || !loaded_files ||
       loaded_files->count == 0 || !loaded_files->files) throw Issue::DatabaseLoadFailure;
    if(semantic != admitted_core || *record_name != semantic[0] + "-" + semantic[2]) throw Issue::MetadataMismatch;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::AfterRecordSelection);
#endif
    // Retain the very descriptors used for core-field discovery. Opening the
    // name again here would turn that validation into adoption of a new object.
    auto& record = *selected_record;
    auto& desc = *selected_desc;
    auto& files = *selected_files;
    auto& mtree = *selected_mtree;
    record.reprove();
    desc.reprove();
    const auto generation = record_generation(record.descriptor.get());
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::AfterRecordOpen);
#endif
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::BeforeRawRead);
#endif
    const auto raw_desc = read_raw(desc, INSTALLED_RECORD_MAXIMUM_METADATA_BYTES, true);
    const auto raw_files = read_raw(files, INSTALLED_RECORD_MAXIMUM_METADATA_BYTES, true);
    const auto raw_mtree = read_raw(mtree, INSTALLED_RECORD_MAXIMUM_MTREE_BYTES, false);
    if(raw_mtree.empty()) throw Issue::MalformedMetadata;
    if(raw_desc != admitted_desc || raw_files != admitted_files) throw Issue::RecordChanged;
    if(desc_core(raw_desc) != semantic) throw Issue::MetadataMismatch;
    std::string database_preimage("desc\0", 5);
    database_preimage += raw_desc;
    database_preimage.append("files\0", 6).append(raw_files);
    InstalledPackageRecordSnapshot snapshot{
        semantic[0], semantic[1], semantic[2], semantic[3], generation,
        xdg_generation_store_raw_contents_sha256(raw_mtree), xdg_generation_store_raw_contents_sha256(database_preimage),
        record.identity.encode() + ";" + desc.identity.encode() + ";" + files.identity.encode() + ";" + mtree.identity.encode()};
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    event(InstalledRecordObservationTestEvent::AfterRawRead);
    event(InstalledRecordObservationTestEvent::BeforeFinalReproof);
#endif
    if(record_generation(record.descriptor.get()) != generation) throw Issue::GenerationMismatch;
    desc.reprove();
    files.reprove();
    mtree.reprove();
    record.reprove();
    database.reprove();
    if(entries_before != relevant_entries(database.local(), name)) throw Issue::RecordChanged;
    return snapshot;
}

std::string fixed_database_configuration() {
    auto root = open_entry(AT_FDCWD, "/", true, false, true);
    auto usr = open_entry(root.descriptor.get(), "usr", true, false, true);
    auto bin = open_entry(usr.descriptor.get(), "bin", true, false, true);
    auto executable = open_entry(bin.descriptor.get(), "pacman-conf", false, true, true, true);
    auto etc = open_entry(root.descriptor.get(), "etc", true, false, true);
    auto config = open_entry(etc.descriptor.get(), "pacman.conf", false, true, true);
    int pipe_fds[2];
    if(pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) throw Issue::UnsupportedDatabaseWorld;
    Descriptor input(pipe_fds[0]), output(pipe_fds[1]);
    const pid_t child = fork();
    if(child < 0) throw Issue::UnsupportedDatabaseWorld;
    if(child == 0) {
        if(dup2(output.get(), STDOUT_FILENO) < 0) _exit(127);
        static_cast<void>(fcntl(STDOUT_FILENO, F_SETFL, 0));
        char command[] = "pacman-conf", option[] = "--config", config_path[] = "/etc/pacman.conf";
        char verbose[] = "--verbose", root_key[] = "RootDir", db_key[] = "DBPath";
        char locale[] = "LC_ALL=C", path[] = "PATH=/usr/bin:/bin";
        char* arguments[] = {command, option, config_path, verbose, root_key, db_key, nullptr};
        char* environment[] = {locale, path, nullptr};
        fexecve(executable.descriptor.get(), arguments, environment);
        _exit(127);
    }
    output = Descriptor{};
    std::string result;
    bool child_reaped = false;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::array<char, 4096> buffer{};
        bool eof = false;
        while(!eof) {
            if(std::chrono::steady_clock::now() >= deadline) throw Issue::UnsupportedDatabaseWorld;
            struct pollfd waiting{input.get(), POLLIN | POLLHUP, 0};
            const int ready = poll(&waiting, 1, 50);
            if(ready < 0 && errno == EINTR) continue;
            if(ready < 0) throw Issue::UnsupportedDatabaseWorld;
            if(ready == 0) continue;
            const ssize_t count = read(input.get(), buffer.data(), buffer.size());
            if(count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if(count < 0) throw Issue::UnsupportedDatabaseWorld;
            if(count == 0) {
                eof = true;
                continue;
            }
            if(result.size() + static_cast<std::size_t>(count) > 16384) throw Issue::UnsupportedDatabaseWorld;
            result.append(buffer.data(), static_cast<std::size_t>(count));
        }
        int status = 0;
        while(true) {
            const auto waited = waitpid(child, &status, WNOHANG);
            if(waited == child) {
                child_reaped = true;
                break;
            }
            if(waited < 0 && errno != EINTR) throw Issue::UnsupportedDatabaseWorld;
            if(std::chrono::steady_clock::now() >= deadline) throw Issue::UnsupportedDatabaseWorld;
            static_cast<void>(poll(nullptr, 0, 10));
        }
        if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw Issue::UnsupportedDatabaseWorld;
        executable.reprove();
        bin.reprove();
        usr.reprove();
        config.reprove();
        etc.reprove();
        root.reprove();
        return result;
    } catch(...) {
        if(!child_reaped) {
            static_cast<void>(kill(child, SIGKILL));
            while(waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        throw;
    }
}

constexpr std::array<std::string_view, 19> ISSUE_NAMES = {
    "UnsupportedDatabaseWorld", "DatabaseWorldMismatch", "DatabaseLoadFailure", "MissingPackage",
    "MalformedMetadata", "MetadataMismatch", "UnsafeRecord", "RecordChanged", "ReadFailure", "MetadataTooLarge",
    "UnsupportedGeneration", "GenerationMismatch", "MtreeMismatch", "RecordDigestMismatch", "MissingBaseline",
    "MissingAnchor", "OperationMismatch", "ResourceFailure", "invalid"};

std::string issue_text(Issue issue) {
    const auto index = static_cast<std::size_t>(issue);
    if(index >= ISSUE_NAMES.size() - 1) throw Issue::MalformedMetadata;
    return std::string(ISSUE_NAMES[index]);
}

Issue parse_issue(std::string_view value) {
    const auto found = std::find(ISSUE_NAMES.begin(), ISSUE_NAMES.end() - 1, value);
    if(found == ISSUE_NAMES.end() - 1) throw Issue::MalformedMetadata;
    return static_cast<Issue>(found - ISSUE_NAMES.begin());
}

std::vector<std::string_view> lines(std::string_view protocol) {
    if(protocol.size() > MAX_PROTOCOL_BYTES || protocol.empty() || protocol.back() != '\n' ||
       protocol.find('\0') != std::string_view::npos) throw Issue::MalformedMetadata;
    std::vector<std::string_view> result;
    while(!protocol.empty()) {
        const auto end = protocol.find('\n');
        result.push_back(protocol.substr(0, end));
        protocol.remove_prefix(end + 1);
        if(result.size() > 16) throw Issue::MalformedMetadata;
    }
    return result;
}

std::string value(std::string_view line, std::string_view key) {
    if(!line.starts_with(key)) throw Issue::MalformedMetadata;
    return decode_hex(line.substr(key.size()));
}

} // namespace

InstalledDatabaseWorldResult resolve_trusted_installed_database_world() noexcept {
    try {
        std::string database_path;
#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        if(g_test_hooks.database_path)
            database_path = *g_test_hooks.database_path;
        else
#endif
        {
            const auto configuration = fixed_database_configuration();
            constexpr std::string_view PREFIX = "RootDir = /\nDBPath = ";
            if(!configuration.starts_with(PREFIX) || configuration.back() != '\n') throw Issue::UnsupportedDatabaseWorld;
            database_path = configuration.substr(PREFIX.size(), configuration.size() - PREFIX.size() - 1);
            if(database_path.find('\n') != std::string::npos) throw Issue::UnsupportedDatabaseWorld;
            if(database_path.size() > 1 && database_path.back() == '/') database_path.pop_back();
        }
        DatabaseDescriptors descriptors(database_path);
        return InstalledDatabaseWorld{"/", std::move(database_path), descriptors.identity()};
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::UnsupportedDatabaseWorld;
    }
}

InstalledPackageRecordObservation observe_installed_package_record(
    const InstalledDatabaseWorld& world, const std::string& package_name) noexcept {
    try {
        return observe_record(world, package_name);
    } catch(Issue issue) {
        return issue;
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::ReadFailure;
    }
}

std::string serialize_installed_database_world(const InstalledDatabaseWorldResult& world) {
    std::string protocol = "MOGUET-INSTALLED-DATABASE-WORLD\t1\n";
    if(const auto* issue = std::get_if<Issue>(&world)) return protocol + "FAILURE\t" + issue_text(*issue) + "\nEND\n";
    const auto& known = std::get<InstalledDatabaseWorld>(world);
    return protocol + "ROOT\t" + hex_bytes(known.root_directory) + "\nDB\t" + hex_bytes(known.database_path) +
           "\nIDENTITY\t" + hex_bytes(known.descriptor_identity) + "\nEND\n";
}

InstalledDatabaseWorldResult parse_installed_database_world(std::string_view protocol) noexcept {
    try {
        const auto records = lines(protocol);
        if(records.front() != "MOGUET-INSTALLED-DATABASE-WORLD\t1" || records.back() != "END") throw Issue::MalformedMetadata;
        if(records.size() == 3 && records[1].starts_with("FAILURE\t")) return parse_issue(records[1].substr(8));
        if(records.size() != 5) throw Issue::MalformedMetadata;
        InstalledDatabaseWorld world{value(records[1], "ROOT\t"), value(records[2], "DB\t"), value(records[3], "IDENTITY\t")};
        if(world.root_directory != "/" || world.database_path.empty() || world.database_path.front() != '/' ||
           world.database_path.find('\0') != std::string::npos || world.descriptor_identity.empty() ||
           serialize_installed_database_world(world) != protocol) throw Issue::MalformedMetadata;
        return world;
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::MalformedMetadata;
    }
}

std::string serialize_installed_package_record_observation(const InstalledPackageRecordObservation& observation) {
    std::string protocol = "MOGUET-INSTALLED-PACKAGE-RECORD\t1\n";
    if(const auto* issue = std::get_if<Issue>(&observation)) return protocol + "FAILURE\t" + issue_text(*issue) + "\nEND\n";
    if(std::holds_alternative<InstalledPackageRecordAbsent>(observation)) return protocol + "ABSENT\nEND\n";
    const auto& record = std::get<InstalledPackageRecordSnapshot>(observation);
    return protocol + "NAME\t" + hex_bytes(record.package_name) + "\nBASE\t" + hex_bytes(record.package_base) +
           "\nVERSION\t" + hex_bytes(record.full_version) + "\nARCH\t" + hex_bytes(record.architecture) +
           "\nGENERATION\t" + hex_bytes(record.record_generation) + "\nMTREE\t" + record.raw_mtree_sha256 +
           "\nDATABASE\t" + record.raw_database_sha256 + "\nIDENTITY\t" + hex_bytes(record.descriptor_identity) + "\nEND\n";
}

InstalledPackageRecordObservation parse_installed_package_record_observation(std::string_view protocol) noexcept {
    try {
        const auto records = lines(protocol);
        if(records.front() != "MOGUET-INSTALLED-PACKAGE-RECORD\t1" || records.back() != "END") throw Issue::MalformedMetadata;
        if(records.size() == 3) {
            if(records[1] == "ABSENT") return InstalledPackageRecordAbsent{};
            if(records[1].starts_with("FAILURE\t")) return parse_issue(records[1].substr(8));
            throw Issue::MalformedMetadata;
        }
        if(records.size() != 10 || !records[6].starts_with("MTREE\t") || !records[7].starts_with("DATABASE\t")) throw Issue::MalformedMetadata;
        InstalledPackageRecordSnapshot record{
            value(records[1], "NAME\t"), value(records[2], "BASE\t"), value(records[3], "VERSION\t"), value(records[4], "ARCH\t"),
            value(records[5], "GENERATION\t"), std::string(records[6].substr(6)), std::string(records[7].substr(9)), value(records[8], "IDENTITY\t")};
        if(!is_valid_package_name(record.package_name) || !is_valid_package_name(record.package_base) ||
           !field_valid(record.full_version) || !field_valid(record.architecture) || record.record_generation.empty() ||
           record.record_generation.size() > 1024 || record.record_generation.find('\0') != std::string::npos ||
           record.descriptor_identity.empty() || !is_valid_source_artifact_install_sha256(record.raw_mtree_sha256) ||
           !is_valid_source_artifact_install_sha256(record.raw_database_sha256) ||
           serialize_installed_package_record_observation(record) != protocol) throw Issue::MalformedMetadata;
        return record;
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::MalformedMetadata;
    }
}

#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
void set_installed_record_observation_test_hooks(InstalledRecordObservationTestHooks hooks) {
    g_test_hooks = std::move(hooks);
}
std::variant<std::string, InstalledRecordObservationIssue> observe_installed_record_generation_for_test(int descriptor) noexcept {
    try {
        return record_generation(descriptor);
    } catch(Issue issue) {
        return issue;
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::UnsupportedGeneration;
    }
}
#endif
