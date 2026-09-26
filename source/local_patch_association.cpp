#include "local_patch_association.hpp"

#include "local_source_metadata_evaluation.hpp"
#include "source_install.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_generation_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <toml++/toml.hpp>

struct PatchAssociationDirectoryAccess {
    static int descriptor(const xdg_directory_safety::PreparedDirectory& directory) {
        directory.require_unchanged_identity();
        return directory.directory_descriptor_;
    }
};

struct LoadedPatchAssociation::Observation {
    std::filesystem::path path;
    struct stat status;
    std::string bytes;
};

struct PatchAssociationAccess {
    static void require_outside_source(const ObservedLocalPatchSource& source,
                                       std::uintmax_t device, std::uintmax_t inode) {
        require_directory_identity_outside_local_source_tree(source.original_, device, inode);
    }
    static ObservedLocalPatchSource source(LocalSourceRoot root, PackageBaseIdentity identity) {
        return ObservedLocalPatchSource(std::move(root), std::move(identity));
    }
    static LoadedPatchAssociation loaded(std::filesystem::path path, const struct stat& status, int version,
                                         std::string bytes, PackageBaseIdentity identity, std::filesystem::path root,
                                         std::vector<PatchMaterialEntry> entries) {
        return LoadedPatchAssociation(std::make_shared<LoadedPatchAssociation::Observation>(
                                          LoadedPatchAssociation::Observation{std::move(path), status, std::move(bytes)}),
                                      version, std::move(identity), std::move(root), std::move(entries));
    }
    static const auto& observation(const LoadedPatchAssociation& loaded) {
        return *loaded.observation_;
    }
    static AcquiredLocalRecipeSeries acquired(PackageBaseIdentity identity, std::vector<LocalRecipePatch> patches) {
        return AcquiredLocalRecipeSeries(std::move(identity), std::move(patches));
    }
};

namespace {
namespace fs = std::filesystem;
using Kind = PatchAssociationFailureKind;
constexpr std::size_t MAX_RECORD = 65536;
constexpr std::size_t MAX_PATCH = 16U * 1024U * 1024U;
constexpr std::size_t MAX_SERIES = 64U * 1024U * 1024U;
constexpr std::size_t MAX_ENTRIES = 64;
constexpr int LOCAL_SCHEMA_VERSION = 1;
constexpr int AUR_SCHEMA_VERSION = 2;

class Failure final : public std::exception {
public:
    PatchAssociationFailure value;
    explicit Failure(PatchAssociationFailure failure) : value(std::move(failure)) {
    }
};
[[noreturn]] void fail(Kind kind, const fs::path& path, int error = 0) {
    throw Failure({kind, path, std::error_code(error, std::generic_category()), std::nullopt, std::nullopt});
}

#ifdef MOGUET_ENABLE_PATCH_ASSOCIATION_TEST_HOOKS
PatchAssociationTestHook g_test_hook;
std::optional<PatchAssociationTestPoint> g_failure_point;
void event(PatchAssociationTestPoint point, const fs::path& path) {
    if(g_test_hook) g_test_hook(point, path);
    if(g_failure_point == point) {
        g_failure_point.reset();
        fail(Kind::IoFailure, path, EIO);
    }
}
#define PATCH_EVENT(point, path) event(PatchAssociationTestPoint::point, path)
#else
#define PATCH_EVENT(point, path) static_cast<void>(0)
#endif

class Descriptor final {
    int fd_ = -1;

public:
    explicit Descriptor(int fd = -1) : fd_(fd) {
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {
    }
    ~Descriptor() {
        if(fd_ >= 0) ::close(fd_);
    }
    int get() const noexcept {
        return fd_;
    }
    int release() noexcept {
        return std::exchange(fd_, -1);
    }
};

struct DirectoryCloser {
    void operator()(DIR* stream) const noexcept {
        static_cast<void>(::closedir(stream));
    }
};

bool same_object(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_uid == b.st_uid && a.st_mode == b.st_mode;
}
bool same_file(const struct stat& a, const struct stat& b, bool renamed = false) {
    return same_object(a, b) && a.st_nlink == b.st_nlink && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           (renamed || (a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec));
}
struct stat status_of(int fd, const fs::path& path) {
    struct stat status{};
    if(::fstat(fd, &status) != 0) fail(Kind::IoFailure, path, errno);
    return status;
}
std::optional<struct stat> named_status(int parent, const std::string& name, const fs::path& path) {
    struct stat status{};
    if(::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) return status;
    if(errno == ENOENT) return std::nullopt;
    fail(Kind::IoFailure, path, errno);
}
bool safe_text(std::string_view text) {
    return !text.empty() && std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool absolute_path(const fs::path& path) {
    const auto value = path.string();
    return safe_text(value) && path.is_absolute() && value != "/" &&
           !value.starts_with("//") && path.lexically_normal().string() == value &&
           !path.filename().empty();
}
bool valid_leaf(const std::string& name) {
    return safe_text(name) && name.size() <= 255 && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}
void require_local_identity(const PackageBaseIdentity& identity) {
    const auto& source = identity.source();
    if(source.kind() != PackageSourceKind::Local || source.location().state() != SourceLocationState::Known ||
       source.location().value() == nullptr || !absolute_path(*source.location().value()))
        fail(Kind::InvalidIdentity, {});
}
std::string canonical_aur_url(const std::string& package_base) {
    // Reuse the checkout owner's construction rule, never parse a display key
    // or accept arbitrary URL aliases as another spelling of the same source.
    return ResolvedAurSourceBuildIdentity(package_base, package_base).checkout().git_url();
}
void require_identity(const PackageBaseIdentity& identity) {
    const auto& source = identity.source();
    if(source.kind() == PackageSourceKind::Local) {
        require_local_identity(identity);
        return;
    }
    if(source.kind() != PackageSourceKind::Aur ||
       source.location().state() != SourceLocationState::Known || !source.location().value())
        fail(Kind::InvalidIdentity, {});
    if(*source.location().value() != canonical_aur_url(identity.package_base()))
        fail(Kind::AssociationMismatch, {});
}
std::string record_leaf(const PackageBaseIdentity& identity) {
    require_identity(identity);
    // Preserve every byte of the v1 derivation. AUR has a separate domain and
    // explicit kind; validated fields cannot contain the NUL separators.
    const bool local = identity.source().kind() == PackageSourceKind::Local;
    std::string key = local ? "moguet-local-recipe-patch-v1" : "moguet-aur-recipe-patch-v2";
    if(!local) {
        key.push_back('\0');
        key += "aur";
    }
    key.push_back('\0');
    key += *identity.source().location().value();
    key.push_back('\0');
    key += identity.package_base();
    return xdg_generation_store_raw_contents_sha256(key) + ".toml";
}
void require_names(const std::vector<std::string>& names, const fs::path& path) {
    if(names.empty() || names.size() > MAX_ENTRIES) fail(Kind::InvalidMaterial, path);
    std::set<std::string> unique;
    for(const auto& name : names)
        if(!valid_leaf(name) || !unique.insert(name).second) fail(Kind::InvalidMaterial, path / name);
}
void require_file(const struct stat& status, const fs::path& path, bool config) {
    if(!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
       (status.st_mode & 0022) != 0 || (status.st_mode & 07000) != 0 ||
       (config && ((status.st_mode & 0777) != 0600 || status.st_nlink != 1))) fail(Kind::Unsafe, path);
}

struct OpenFile {
    Descriptor descriptor;
    struct stat status;
    std::string bytes;
};
void revalidate_file(int parent, const std::string& name, const fs::path& path,
                     const OpenFile& file, bool config, bool renamed = false) {
    const auto held = status_of(file.descriptor.get(), path);
    require_file(held, path, config);
    const auto named = named_status(parent, name, path);
    if(!named || !same_file(file.status, held, renamed) || !same_file(held, *named)) fail(Kind::ConcurrentChange, path);
}
std::string read_bytes(int fd, const struct stat& expected, std::size_t limit, const fs::path& path, bool material = false) {
    if(expected.st_size < 0 || static_cast<std::uintmax_t>(expected.st_size) > limit)
        fail(Kind::InvalidMaterial, path);
    std::string bytes;
    std::array<char, 32768> buffer{};
    while(bytes.size() < static_cast<std::size_t>(expected.st_size)) {
        const auto count = ::pread(fd, buffer.data(), std::min(buffer.size(), static_cast<std::size_t>(expected.st_size) - bytes.size()),
                                   static_cast<off_t>(bytes.size()));
        if(count < 0 && errno == EINTR) continue;
        if(count < 0) fail(Kind::IoFailure, path, errno);
        if(count == 0) fail(Kind::ConcurrentChange, path);
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
        if(material) PATCH_EVENT(PartialRead, path);
    }
    char extra{};
    ssize_t count;
    do {
        count = ::pread(fd, &extra, 1, expected.st_size);
    } while(count < 0 && errno == EINTR);
    if(count < 0) fail(Kind::IoFailure, path, errno);
    if(count != 0) fail(Kind::ConcurrentChange, path);
    return bytes;
}
std::optional<OpenFile> open_file(int parent, const std::string& name, const fs::path& path,
                                  std::size_t limit, bool config) {
    const auto named = named_status(parent, name, path);
    if(!named) return std::nullopt;
    require_file(*named, path, config);
    Descriptor fd(::openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if(fd.get() < 0) fail(errno == ELOOP || errno == ENOENT ? Kind::ConcurrentChange : Kind::IoFailure, path, errno);
    auto held = status_of(fd.get(), path);
#ifdef MOGUET_ENABLE_PATCH_ASSOCIATION_TEST_HOOKS
    if(!config && g_failure_point == PatchAssociationTestPoint::WrongMaterialOwner) {
        g_failure_point.reset();
        held.st_uid = ::geteuid() == 0 ? 1 : 0;
    }
#endif
    require_file(held, path, config);
    if(config && (held.st_size < 0 || static_cast<std::uintmax_t>(held.st_size) > MAX_RECORD)) fail(Kind::Corrupt, path);
    if(!same_file(*named, held)) fail(Kind::ConcurrentChange, path);
    if(!config) {
        PATCH_EVENT(AfterMaterialOpen, path);
    }
    auto bytes = read_bytes(fd.get(), held, limit, path, !config);
    if(!config) PATCH_EVENT(AfterMaterialRead, path);
    if(config) PATCH_EVENT(AfterRecordRead, path);
    OpenFile file{std::move(fd), held, std::move(bytes)};
    revalidate_file(parent, name, path, file, config);
    return file;
}

// All root-to-material links stay descriptor-pinned until the entire series
// proof completes. Root-owned /tmp with sticky bit is allowed as an ancestor;
// the material root itself must be euid-owned and non-shared-writable.
class MaterialDirectory final {
    struct Link {
        Descriptor fd;
        std::string name;
        struct stat status;
    };
    std::vector<Link> links_;
    fs::path path_;
    static void check(const struct stat& status, const fs::path& path, bool material_root) {
        const bool sticky_system = !material_root && status.st_uid == 0 && (status.st_mode & S_ISVTX) != 0;
        if(!S_ISDIR(status.st_mode) || (status.st_uid != ::geteuid() && (material_root || status.st_uid != 0)) ||
           ((status.st_mode & 0022) != 0 && !sticky_system) || (status.st_mode & (S_ISUID | S_ISGID)) != 0)
            fail(Kind::Unsafe, path);
    }

public:
    explicit MaterialDirectory(fs::path path) : path_(std::move(path)) {
        if(!absolute_path(path_)) fail(Kind::InvalidMaterial, path_);
        Descriptor root(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if(root.get() < 0) fail(Kind::IoFailure, path_, errno);
        auto status = status_of(root.get(), path_);
        check(status, "/", false);
        links_.push_back({std::move(root), {}, status});
        fs::path partial = "/";
        for(const auto& component : path_.relative_path()) {
            partial /= component;
            const auto named = named_status(links_.back().fd.get(), component.string(), partial);
            if(!named) fail(Kind::Missing, partial);
            check(*named, partial, partial == path_);
            Descriptor fd(::openat(links_.back().fd.get(), component.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if(fd.get() < 0) fail(Kind::IoFailure, partial, errno);
            status = status_of(fd.get(), partial);
            if(!same_object(*named, status)) fail(Kind::ConcurrentChange, partial);
            links_.push_back({std::move(fd), component.string(), status});
        }
        revalidate();
    }
    int fd() const {
        return links_.back().fd.get();
    }
    void revalidate() const {
        for(std::size_t i = 0; i < links_.size(); ++i) {
            const auto held = status_of(links_[i].fd.get(), path_);
            check(held, path_, i + 1 == links_.size());
            if(!same_object(held, links_[i].status)) fail(Kind::ConcurrentChange, path_);
            if(i > 0) {
                const auto named = named_status(links_[i - 1].fd.get(), links_[i].name, path_);
                if(!named || !same_object(held, *named)) fail(Kind::ConcurrentChange, path_);
            }
        }
    }
};

struct MaterialSnapshot {
    std::vector<PatchMaterialEntry> entries;
    std::vector<LocalRecipePatch> patches;
};
MaterialSnapshot acquire_material(const fs::path& root, const std::vector<std::string>& names,
                                  const std::vector<PatchMaterialEntry>* expected) {
    require_names(names, root);
    MaterialDirectory directory(root);
    std::vector<OpenFile> files;
    std::set<std::pair<dev_t, ino_t>> unique;
    std::size_t total = 0;
    for(const auto& name : names) {
        auto file = open_file(directory.fd(), name, root / name, MAX_PATCH, false);
        if(!file) fail(Kind::Missing, root / name);
        if(!unique.emplace(file->status.st_dev, file->status.st_ino).second) fail(Kind::InvalidMaterial, root / name);
        total += file->bytes.size();
        if(total > MAX_SERIES) fail(Kind::InvalidMaterial, root);
        files.push_back(std::move(*file));
    }
    PATCH_EVENT(AfterSeriesRead, root);
    MaterialSnapshot result;
    for(std::size_t i = 0; i < files.size(); ++i) {
        revalidate_file(directory.fd(), names[i], root / names[i], files[i], false);
        const auto digest = xdg_generation_store_raw_contents_sha256(files[i].bytes);
        if(expected && (*expected)[i].sha256 != digest) fail(Kind::Changed, root / names[i]);
        if(validate_local_recipe_patch(files[i].bytes)) fail(Kind::InvalidMaterial, root / names[i]);
        result.entries.push_back({names[i], digest});
        result.patches.push_back({std::move(files[i].bytes)});
    }
    for(std::size_t i = 0; i < files.size(); ++i)
        revalidate_file(directory.fd(), names[i], root / names[i], files[i], false);
    directory.revalidate();
    return result;
}

std::string encode(const PackageBaseIdentity& identity, const fs::path& root,
                   const std::vector<PatchMaterialEntry>& entries) {
    toml::array patches;
    for(const auto& entry : entries)
        patches.push_back(toml::table{{"file", entry.file}, {"sha256", entry.sha256}});
    const bool local = identity.source().kind() == PackageSourceKind::Local;
    const toml::table record{{"schema_version", local ? LOCAL_SCHEMA_VERSION : AUR_SCHEMA_VERSION}, {"source_kind", local ? "local" : "aur"}, {local ? "local_source" : "source_url", *identity.source().location().value()}, {"package_base", identity.package_base()}, {"material_root", root.string()}, {"patches", std::move(patches)}};
    std::ostringstream output;
    output << record << '\n';
    auto bytes = output.str();
    // Reject a native filename that cannot be represented as valid TOML/UTF-8
    // before it can create an unreadable persistent record.
    try {
        static_cast<void>(toml::parse(bytes));
    } catch(const toml::parse_error&) {
        fail(Kind::InvalidMaterial, root);
    }
    return bytes;
}
PackageBaseIdentity decode_local_v1(const toml::table& table, const fs::path& path) {
    const auto kind = table["source_kind"].value<std::string>();
    if(!kind) fail(Kind::Corrupt, path);
    if(*kind != "local") fail(Kind::Unsupported, path);
    const auto source = table["local_source"].value<std::string>();
    const auto base = table["package_base"].value<std::string>();
    if(table.size() != 6 || !source || !base || !absolute_path(*source)) fail(Kind::Corrupt, path);
    return PackageBaseIdentity::make(PackageSourceIdentity::local(SourceLocationIdentity::known_local_path(*source)), *base);
}
PackageBaseIdentity decode_aur_v2(const toml::table& table, const fs::path& path) {
    const auto kind = table["source_kind"].value<std::string>();
    if(!kind) fail(Kind::Corrupt, path);
    // v2 is AUR-only: a second encoding of local identities would create
    // duplicate/migration ambiguity. Local writers continue to emit v1.
    if(*kind != "aur") fail(Kind::Unsupported, path);
    const auto source = table["source_url"].value<std::string>();
    const auto base = table["package_base"].value<std::string>();
    if(table.size() != 6 || !source || !base) fail(Kind::Corrupt, path);
    auto identity = PackageBaseIdentity::make(PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote(*source)), *base);
    if(*source != canonical_aur_url(*base)) fail(Kind::AssociationMismatch, path);
    return identity;
}
LoadedPatchAssociation decode(const fs::path& path, const OpenFile& raw, const PackageBaseIdentity* expected = nullptr) {
    try {
        const auto table = toml::parse(raw.bytes);
        const auto version = table["schema_version"].value_exact<std::int64_t>();
        if(!version) fail(Kind::Corrupt, path);
        if(*version != LOCAL_SCHEMA_VERSION && *version != AUR_SCHEMA_VERSION) fail(Kind::Unsupported, path);
        auto identity = *version == LOCAL_SCHEMA_VERSION ? decode_local_v1(table, path) : decode_aur_v2(table, path);
        const auto root = table["material_root"].value<std::string>();
        const auto* patches = table["patches"].as_array();
        if(!root || !patches || !absolute_path(*root)) fail(Kind::Corrupt, path);
        // Both exact lookup and discovery must bind the decoded identity to
        // its actual registry key. No source/material filesystem lookup here.
        if(path.filename() != record_leaf(identity) || (expected && identity != *expected)) fail(Kind::AssociationMismatch, path);
        std::vector<PatchMaterialEntry> entries;
        std::vector<std::string> names;
        for(const auto& patch : *patches) {
            const auto* entry = patch.as_table();
            if(!entry || entry->size() != 2) fail(Kind::Corrupt, path);
            const auto file = (*entry)["file"].value<std::string>();
            const auto digest = (*entry)["sha256"].value<std::string>();
            if(!file || !digest || digest->size() != 64 || !std::all_of(digest->begin(), digest->end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) fail(Kind::Corrupt, path);
            names.push_back(*file);
            entries.push_back({*file, *digest});
        }
        try {
            require_names(names, path);
        } catch(const Failure&) {
            fail(Kind::Corrupt, path);
        }
        return PatchAssociationAccess::loaded(path, raw.status, static_cast<int>(*version), raw.bytes, std::move(identity), *root, std::move(entries));
    } catch(const toml::parse_error&) {
        fail(Kind::Corrupt, path);
    } catch(const std::invalid_argument&) {
        fail(Kind::Corrupt, path);
    }
}

struct LockedDirectory {
    xdg_directory_safety::PreparedDirectory directory;
    Descriptor lock;
    LockedDirectory(xdg_directory_safety::PreparedDirectory input, bool write)
        : directory(std::move(input)), lock(::openat(PatchAssociationDirectoryAccess::descriptor(directory), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
        if(lock.get() < 0) fail(Kind::IoFailure, directory.path(), errno);
        int result;
        do {
            result = ::flock(lock.get(), write ? LOCK_EX : LOCK_SH);
        } while(result != 0 && errno == EINTR);
        if(result != 0) fail(Kind::IoFailure, directory.path(), errno);
        directory.require_unchanged_identity();
    }
    int fd() const {
        return lock.get();
    }
};

std::vector<std::string> directory_names(LockedDirectory& directory) {
    Descriptor scan(::openat(directory.fd(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if(scan.get() < 0) fail(Kind::IoFailure, directory.directory.path(), errno);
    std::unique_ptr<DIR, DirectoryCloser> stream(::fdopendir(scan.get()));
    if(!stream) fail(Kind::IoFailure, directory.directory.path(), errno);
    static_cast<void>(scan.release());
    std::vector<std::string> names;
    int read_error = 0;
    while(true) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if(!entry) {
            read_error = errno;
            break;
        }
        const std::string name = entry->d_name;
        if(name != "." && name != "..") names.push_back(name);
    }
    const int close_result = ::closedir(stream.release());
    if(read_error || close_result != 0) fail(Kind::IoFailure, directory.directory.path(), read_error ? read_error : errno);
    directory.directory.require_unchanged_identity();
    std::sort(names.begin(), names.end());
    return names;
}
void require_no_residue(LockedDirectory& directory, const std::string& leaf) {
    for(const auto& name : directory_names(directory))
        if(name.starts_with("." + leaf + "-")) fail(Kind::Unsafe, directory.directory.path() / name);
}
std::optional<OpenFile> read_record(LockedDirectory& directory, const std::string& leaf) {
    require_no_residue(directory, leaf);
    auto record = open_file(directory.fd(), leaf, directory.directory.path() / leaf, MAX_RECORD, true);
    directory.directory.require_unchanged_identity();
    return record;
}
void require_previous(const LoadedPatchAssociation& previous, const fs::path& path,
                      const std::optional<OpenFile>& current) {
    const auto& observation = PatchAssociationAccess::observation(previous);
    if(!current || path != observation.path || current->bytes != observation.bytes ||
       !same_file(current->status, observation.status)) fail(Kind::ConcurrentChange, path);
}
std::string temporary_leaf(const std::string& leaf) {
    std::array<unsigned char, 16> random{};
    if(::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size())) fail(Kind::IoFailure, {}, errno);
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "." + leaf + "-";
    for(auto byte : random) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

PatchAssociationFailure map_exception() {
    try {
        throw;
    } catch(const Failure& error) {
        return error.value;
    } catch(const xdg_paths::ResolutionError&) {
        return {Kind::Unsafe, {}, {}, std::nullopt, std::nullopt};
    } catch(const xdg_directory_safety::PreparationError& error) {
        const auto code = error.failure().code;
        const bool io = code == xdg_directory_safety::PreparationErrorCode::PermissionDenied ||
                        code == xdg_directory_safety::PreparationErrorCode::MetadataFailure || code == xdg_directory_safety::PreparationErrorCode::CreationFailed;
        return {io ? Kind::IoFailure : Kind::Unsafe, {}, error.failure().system_error.value_or(std::error_code{}), std::nullopt, std::nullopt};
    } catch(const std::system_error& error) {
        return {Kind::IoFailure, {}, error.code(), std::nullopt, std::nullopt};
    } catch(const LocalSourceRootError& error) {
        return {Kind::ConcurrentChange, error.failure().path, {}, std::nullopt, std::nullopt};
    } catch(const LocalSourceWorkspaceError& error) {
        return {Kind::Unsafe, error.failure().relative_path, error.failure().system_error.value_or(std::error_code{}), std::nullopt, std::nullopt};
    } catch(...) {
        return {Kind::IoFailure, {}, {}, std::nullopt, std::nullopt};
    }
}

std::vector<LoadedPatchAssociation> read_registry(LockedDirectory& store) {
    const auto initial = status_of(store.fd(), store.directory.path());
    const auto names = directory_names(store);
    PATCH_EVENT(AfterRegistryEnumeration, store.directory.path());
    std::vector<LoadedPatchAssociation> records;
    for(const auto& name : names) {
        const auto path = store.directory.path() / name;
        // A retained publication file cannot become silent absence even
        // when its final record is gone. This namespace contains records only.
        if(name.starts_with('.')) fail(Kind::Unsafe, path);
        if(!name.ends_with(".toml")) fail(Kind::Corrupt, path);
        auto raw = open_file(store.fd(), name, path, MAX_RECORD, true);
        if(!raw) fail(Kind::ConcurrentChange, path);
        records.push_back(decode(path, *raw));
        revalidate_file(store.fd(), name, path, *raw, true);
    }
    // Do not return a partial/mixed snapshot after an observed registry
    // change. The shared directory lock excludes cooperative writers.
    for(const auto& record : records) {
        const auto& observed = PatchAssociationAccess::observation(record);
        const auto named = named_status(store.fd(), observed.path.filename().string(), observed.path);
        if(!named || !same_file(observed.status, *named)) fail(Kind::ConcurrentChange, observed.path);
    }
    if(!same_file(initial, status_of(store.fd(), store.directory.path())))
        fail(Kind::ConcurrentChange, store.directory.path());
    store.directory.require_unchanged_identity();
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        if(a.identity().package_base() != b.identity().package_base())
            return a.identity().package_base() < b.identity().package_base();
        if(a.identity().source().kind() != b.identity().source().kind())
            return a.identity().source().kind() == PackageSourceKind::Local;
        return *a.identity().source().location().value() < *b.identity().source().location().value();
    });
    return records;
}

// One-file publication with a defined commit point. Before rename, old bytes
// remain authority. After rename, an incomplete proof/sync is Uncertain: never
// rollback or claim that an operation definitely did not happen.
PatchAssociationWriteResult publish(const PackageBaseIdentity& identity, const ObservedLocalPatchSource* local_source,
                                    const LoadedPatchAssociation* previous, const fs::path& material_root,
                                    const std::vector<std::string>& names) {
    std::optional<LockedDirectory> directory;
    std::optional<OpenFile> temporary;
    std::string temp;
    bool committed = false;
    try {
        if(local_source) local_source->require_unchanged_identity();
        const auto leaf = record_leaf(identity);
        auto material = acquire_material(material_root, names, nullptr);
        const auto contents = encode(identity, material_root, material.entries);
        if(contents.size() > MAX_RECORD) fail(Kind::InvalidMaterial, material_root);
        const auto paths = xdg_paths::resolve_patch_associations_process_environment();
        const auto within = [](const fs::path& path, const fs::path& root) {
            const auto relative = path.lexically_relative(root);
            return !relative.empty() && *relative.begin() != "..";
        };
        if((local_source && within(paths.directory, *identity.source().location().value())) || within(paths.directory, material_root))
            fail(Kind::Unsafe, paths.directory);
        const auto outside_source = [local_source](const xdg_directory_safety::DirectoryIdentity& parent) {
            if(local_source) PatchAssociationAccess::require_outside_source(*local_source, parent.device, parent.inode);
        };
        if(previous) {
            auto existing = xdg_directory_safety::open_existing_directory(paths);
            if(!existing) fail(Kind::Missing, paths.directory);
            directory.emplace(std::move(*existing), true);
        } else
            directory.emplace(xdg_directory_safety::prepare_directory(paths, outside_source), true);
        auto& store = *directory;
        if(local_source) PatchAssociationAccess::require_outside_source(*local_source, store.directory.device(), store.directory.inode());
        static_cast<void>(read_registry(store));
        const auto path = paths.directory / leaf;
        auto current = read_record(store, leaf);
        if(current) static_cast<void>(decode(path, *current, &identity));
        if(previous) {
            if(previous->identity() != identity) fail(Kind::AssociationMismatch, path);
            require_previous(*previous, path, current);
        } else if(current)
            fail(Kind::AlreadyExists, path);
        temp = temporary_leaf(leaf);
        Descriptor fd(::openat(store.fd(), temp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if(fd.get() < 0) fail(Kind::IoFailure, paths.directory / temp, errno);
        temporary.emplace(OpenFile{std::move(fd), {}, {}});
        if(::fchmod(temporary->descriptor.get(), 0600) != 0) fail(Kind::IoFailure, path, errno);
        PATCH_EVENT(BeforeWrite, path);
        std::size_t offset = 0;
        while(offset < contents.size()) {
            const auto count = ::write(temporary->descriptor.get(), contents.data() + offset, contents.size() - offset);
            if(count < 0 && errno == EINTR) continue;
            if(count <= 0) fail(Kind::IoFailure, path, errno);
            offset += static_cast<std::size_t>(count);
        }
        PATCH_EVENT(BeforeFileSync, path);
        if(::fsync(temporary->descriptor.get()) != 0) fail(Kind::IoFailure, path, errno);
        temporary->status = status_of(temporary->descriptor.get(), path);
        temporary->bytes = contents;
        if(local_source) local_source->require_unchanged_identity();
        PATCH_EVENT(BeforePublication, path);
        if(local_source) local_source->require_unchanged_identity();
        store.directory.require_unchanged_identity();
        revalidate_file(store.fd(), temp, paths.directory / temp, *temporary, true);
        if(current)
            revalidate_file(store.fd(), leaf, path, *current, true);
        else if(named_status(store.fd(), leaf, path))
            fail(Kind::ConcurrentChange, path);
        const unsigned flags = previous ? RENAME_EXCHANGE : RENAME_NOREPLACE;
        if(::renameat2(store.fd(), temp.c_str(), store.fd(), leaf.c_str(), flags) != 0) fail(Kind::IoFailure, path, errno);
        committed = true;
        PATCH_EVENT(AfterPublication, path);
        store.directory.require_unchanged_identity();
        revalidate_file(store.fd(), leaf, path, *temporary, true, true);
        if(current) {
            revalidate_file(store.fd(), temp, paths.directory / temp, *current, true, true);
            if(read_bytes(current->descriptor.get(), status_of(current->descriptor.get(), path), MAX_RECORD, path) != current->bytes)
                fail(Kind::ConcurrentChange, path);
            store.directory.require_unchanged_identity();
            if(::unlinkat(store.fd(), temp.c_str(), 0) != 0) fail(Kind::IoFailure, paths.directory / temp, errno);
        }
        temp.clear();
        PATCH_EVENT(BeforeDirectorySync, path);
        if(::fsync(store.fd()) != 0) fail(Kind::IoFailure, path, errno);
        if(auto error = store.directory.synchronize_managed_parent_entries()) fail(Kind::IoFailure, path, error->value());
        store.directory.require_unchanged_identity();
        auto published = read_record(store, leaf);
        if(!published || published->bytes != contents || !same_object(published->status, temporary->status)) fail(Kind::ConcurrentChange, path);
        return decode(path, *published, &identity);
    } catch(...) {
        auto failure = map_exception();
        if(committed) failure.kind = Kind::PublicationUncertain;
        if(directory && !temp.empty()) {
            const auto temporary_path = directory->directory.path() / temp;
            try {
                if(named_status(directory->fd(), temp, temporary_path)) failure.leftover = temporary_path;
            } catch(...) {
                failure.leftover = temporary_path;
            }
            if(!committed && temporary) {
                // Never unlink a replacement merely because its name was ours.
                try {
                    directory->directory.require_unchanged_identity();
                    const auto held = status_of(temporary->descriptor.get(), temporary_path);
                    require_file(held, temporary_path, true);
                    const auto named = named_status(directory->fd(), temp, temporary_path);
                    if(named && same_object(held, *named) && ::unlinkat(directory->fd(), temp.c_str(), 0) == 0)
                        failure.leftover.reset();
                } catch(...) {
                }
            }
        }
        return failure;
    }
}
} // namespace

ObservedLocalPatchSource::ObservedLocalPatchSource(LocalSourceRoot original, PackageBaseIdentity identity)
    : original_(std::move(original)), identity_(std::move(identity)) {
}
const PackageBaseIdentity& ObservedLocalPatchSource::identity() const noexcept {
    return identity_;
}
void ObservedLocalPatchSource::require_unchanged_identity() const {
    original_.require_unchanged_identity();
}
LoadedPatchAssociation::LoadedPatchAssociation(std::shared_ptr<const Observation> observation, int version, PackageBaseIdentity identity,
                                               fs::path root, std::vector<PatchMaterialEntry> entries) : observation_(std::move(observation)), schema_version_(version), identity_(std::move(identity)),
                                                                                                         material_root_(std::move(root)), entries_(std::move(entries)) {
}
const PackageBaseIdentity& LoadedPatchAssociation::identity() const noexcept {
    return identity_;
}
const fs::path& LoadedPatchAssociation::material_root() const noexcept {
    return material_root_;
}
const std::vector<PatchMaterialEntry>& LoadedPatchAssociation::entries() const noexcept {
    return entries_;
}
int LoadedPatchAssociation::schema_version() const noexcept {
    return schema_version_;
}
AcquiredLocalRecipeSeries::AcquiredLocalRecipeSeries(PackageBaseIdentity identity, std::vector<LocalRecipePatch> patches)
    : identity_(std::move(identity)), patches_(std::move(patches)) {
}
const PackageBaseIdentity& AcquiredLocalRecipeSeries::identity() const noexcept {
    return identity_;
}
const std::vector<LocalRecipePatch>& AcquiredLocalRecipeSeries::patches() const noexcept {
    return patches_;
}
std::vector<LocalRecipePatch> AcquiredLocalRecipeSeries::take_patches() && {
    return std::move(patches_);
}

std::variant<ObservedLocalPatchSource, PatchAssociationFailure> observe_local_patch_source(
    LocalSourceRoot original, const ValidatedCacheRoot& cache_root, SourceBuildEnvironment environment) {
    std::optional<LocalSourceWorkspace> workspace;
    bool cleanup_attempted = false;
    bool evaluating = false;
    try {
        require_unclaimed_artifact_pkgdest(environment);
        workspace.emplace(materialize_local_source_workspace(original, cache_root));
        auto candidate = open_local_source_root(workspace->path(), true);
        auto architecture = resolve_local_source_effective_architecture(environment);
        evaluating = true;
        const auto metadata = evaluate_local_source_metadata(candidate, std::move(environment), std::move(architecture));
        evaluating = false;
        auto identity = PackageBaseIdentity::make(PackageSourceIdentity::local(SourceLocationIdentity::known_local_path(
                                                      original.canonical_path().string())),
                                                  metadata.metadata().package_base);
        original.require_unchanged_identity();
        cleanup_attempted = true;
        workspace->cleanup();
        return PatchAssociationAccess::source(std::move(original), std::move(identity));
    } catch(...) {
        auto failure = map_exception();
        if(evaluating) failure.kind = Kind::ToolFailure;
        if(cleanup_attempted) {
            try {
                throw;
            } catch(const LocalSourceWorkspaceError& e) {
                failure.cleanup_failure = e.failure();
            } catch(...) {
                failure.cleanup_failure = LocalSourceWorkspaceFailure{LocalSourceWorkspaceStage::Cleanup,
                                                                      LocalSourceWorkspaceErrorCode::CleanupFailure,
                                                                      {},
                                                                      std::nullopt};
            }
        }
        if(workspace && !cleanup_attempted) try {
                workspace->cleanup();
            } catch(const LocalSourceWorkspaceError& e) {
                failure.cleanup_failure = e.failure();
            } catch(...) {
                failure.cleanup_failure = LocalSourceWorkspaceFailure{LocalSourceWorkspaceStage::Cleanup,
                                                                      LocalSourceWorkspaceErrorCode::CleanupFailure,
                                                                      {},
                                                                      std::nullopt};
            }
        if(failure.cleanup_failure && workspace) failure.leftover = workspace->path();
        return failure;
    }
}

std::variant<PackageBaseIdentity, PatchAssociationFailure> aur_patch_association_identity(
    const ResolvedAurSourceBuildIdentity& source) {
    try {
        auto identity = PackageBaseIdentity::make(
            PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote(source.checkout().git_url())),
            source.checkout().package_base());
        // The resolved model separates child and base but its constructor does
        // not validate either string. Validate both before accepting the value.
        static_cast<void>(PackageChildIdentity::make(identity, source.requested_name()));
        require_identity(identity);
        return identity;
    } catch(const std::invalid_argument&) {
        return PatchAssociationFailure{Kind::InvalidIdentity, {}, {}, std::nullopt, std::nullopt};
    } catch(...) {
        return map_exception();
    }
}
fs::path patch_association_record_path(const PackageBaseIdentity& identity) {
    const auto leaf = record_leaf(identity);
    return xdg_paths::resolve_patch_associations_process_environment().directory / leaf;
}
fs::path local_patch_association_record_path(const PackageBaseIdentity& identity) {
    require_local_identity(identity);
    return patch_association_record_path(identity);
}
PatchAssociationReadResult read_patch_association(const PackageBaseIdentity& identity) {
    try {
        require_identity(identity);
        auto existing = xdg_directory_safety::open_existing_directory(xdg_paths::resolve_patch_associations_process_environment());
        if(!existing) return PatchAssociationAbsent{};
        LockedDirectory store(std::move(*existing), false);
        // Absence is meaningful only after the complete registry is validated:
        // a malformed/renamed record must not hide a saved association.
        auto records = read_registry(store);
        for(auto& record : records)
            if(record.identity() == identity) return std::move(record);
        return PatchAssociationAbsent{};
    } catch(...) {
        return map_exception();
    }
}
PatchAssociationReadResult read_local_patch_association(const PackageBaseIdentity& identity) {
    try {
        require_local_identity(identity);
        return read_patch_association(identity);
    } catch(...) {
        return map_exception();
    }
}
std::variant<std::vector<LoadedPatchAssociation>, PatchAssociationFailure> list_patch_associations() {
    try {
        auto existing = xdg_directory_safety::open_existing_directory(xdg_paths::resolve_patch_associations_process_environment());
        if(!existing) return std::vector<LoadedPatchAssociation>{};
        LockedDirectory store(std::move(*existing), false);
        return read_registry(store);
    } catch(...) {
        return map_exception();
    }
}
PatchAssociationWriteResult register_local_patch_association(const ObservedLocalPatchSource& source,
                                                             const fs::path& root, const std::vector<std::string>& names) {
    return publish(source.identity(), &source, nullptr, root, names);
}
PatchAssociationWriteResult update_local_patch_association(const ObservedLocalPatchSource& source,
                                                           const LoadedPatchAssociation& previous, const fs::path& root, const std::vector<std::string>& names) {
    return publish(source.identity(), &source, &previous, root, names);
}
PatchAssociationAcquireResult acquire_local_patch_series(const LoadedPatchAssociation& association) {
    try {
        require_local_identity(association.identity());
        const auto leaf = record_leaf(association.identity());
        const auto paths = xdg_paths::resolve_patch_associations_process_environment();
        auto existing = xdg_directory_safety::open_existing_directory(paths);
        if(!existing) fail(Kind::Missing, paths.directory);
        LockedDirectory store(std::move(*existing), false);
        auto raw = read_record(store, leaf);
        if(!raw) fail(Kind::Missing, paths.directory / leaf);
        auto loaded = decode(paths.directory / leaf, *raw, &association.identity());
        require_previous(association, paths.directory / leaf, raw);
        std::vector<std::string> names;
        for(const auto& entry : loaded.entries())
            names.push_back(entry.file);
        auto material = acquire_material(loaded.material_root(), names, &loaded.entries());
        revalidate_file(store.fd(), leaf, paths.directory / leaf, *raw, true);
        store.directory.require_unchanged_identity();
        return PatchAssociationAccess::acquired(loaded.identity(), std::move(material.patches));
    } catch(...) {
        return map_exception();
    }
}

std::variant<PatchAssociationForgotten, PatchAssociationFailure> forget_patch_association(const LoadedPatchAssociation& previous) {
    bool committed = false;
    fs::path leftover;
    try {
        const auto leaf = record_leaf(previous.identity());
        const auto paths = xdg_paths::resolve_patch_associations_process_environment();
        if(previous.identity().source().kind() == PackageSourceKind::Local) {
            const auto relative = paths.directory.lexically_relative(*previous.identity().source().location().value());
            if(!relative.empty() && *relative.begin() != "..") fail(Kind::Unsafe, paths.directory);
        }
        auto existing = xdg_directory_safety::open_existing_directory(paths);
        if(!existing) fail(Kind::Missing, {});
        LockedDirectory store(std::move(*existing), true);
        const auto path = store.directory.path() / leaf;
        auto current = read_record(store, leaf);
        if(current) static_cast<void>(decode(path, *current, &previous.identity()));
        require_previous(previous, path, current);
        const auto temp = temporary_leaf(leaf);
        PATCH_EVENT(BeforePublication, path);
        store.directory.require_unchanged_identity();
        revalidate_file(store.fd(), leaf, path, *current, true);
        if(::renameat2(store.fd(), leaf.c_str(), store.fd(), temp.c_str(), RENAME_NOREPLACE) != 0) fail(Kind::IoFailure, path, errno);
        committed = true;
        leftover = store.directory.path() / temp;
        PATCH_EVENT(AfterPublication, path);
        store.directory.require_unchanged_identity();
        revalidate_file(store.fd(), temp, leftover, *current, true, true);
        if(read_bytes(current->descriptor.get(), status_of(current->descriptor.get(), path), MAX_RECORD, path) != current->bytes)
            fail(Kind::ConcurrentChange, path);
        store.directory.require_unchanged_identity();
        if(::unlinkat(store.fd(), temp.c_str(), 0) != 0) fail(Kind::IoFailure, leftover, errno);
        leftover.clear();
        PATCH_EVENT(BeforeDirectorySync, path);
        if(::fsync(store.fd()) != 0) fail(Kind::IoFailure, path, errno);
        store.directory.require_unchanged_identity();
        if(named_status(store.fd(), leaf, path)) fail(Kind::ConcurrentChange, path);
        return PatchAssociationForgotten{};
    } catch(...) {
        auto failure = map_exception();
        if(committed) failure.kind = Kind::PublicationUncertain;
        if(!leftover.empty()) failure.leftover = leftover;
        return failure;
    }
}

std::variant<PatchAssociationForgotten, PatchAssociationFailure> forget_local_patch_association(const LoadedPatchAssociation& previous) {
    try {
        require_local_identity(previous.identity());
        return forget_patch_association(previous);
    } catch(...) {
        return map_exception();
    }
}
PatchAssociationWriteResult register_aur_patch_association(const ResolvedAurSourceBuildIdentity& source,
                                                           const fs::path& root, const std::vector<std::string>& names) {
    auto identity = aur_patch_association_identity(source);
    if(const auto* failure = std::get_if<PatchAssociationFailure>(&identity)) return *failure;
    return publish(std::get<PackageBaseIdentity>(identity), nullptr, nullptr, root, names);
}
PatchAssociationWriteResult update_aur_patch_association(const ResolvedAurSourceBuildIdentity& source,
                                                         const LoadedPatchAssociation& previous,
                                                         const fs::path& root, const std::vector<std::string>& names) {
    auto identity = aur_patch_association_identity(source);
    if(const auto* failure = std::get_if<PatchAssociationFailure>(&identity)) return *failure;
    return publish(std::get<PackageBaseIdentity>(identity), nullptr, &previous, root, names);
}

#ifdef MOGUET_ENABLE_PATCH_ASSOCIATION_TEST_HOOKS
void set_patch_association_test_hook(PatchAssociationTestHook hook) {
    g_test_hook = std::move(hook);
}
void fail_patch_association_operation_for_test(PatchAssociationTestPoint point) {
    g_failure_point = point;
}
#endif
