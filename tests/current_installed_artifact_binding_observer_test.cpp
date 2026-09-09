#include "current_installed_artifact_binding_observer.hpp"
#include "xdg_generation_store.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <vector>
#include <string_view>
#include <stdexcept>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace allocation {
bool blocked = false;
unsigned failures = 0;
} // namespace allocation
[[gnu::noinline]] void* operator new(std::size_t size) {
    if(allocation::blocked) {
        ++allocation::failures;
        throw std::bad_alloc();
    }
    if(void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
[[gnu::noinline]] void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {
namespace fs = std::filesystem;
using Stage = CurrentInstalledArtifactBindingStage;
using Issue = InstalledRecordObservationIssue;
using Event = InstalledRecordObservationTestEvent;
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
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
struct Fixture {
    fs::path root, db, record;
    InstalledRecordObservationTestHooks hooks;
    unsigned char generation = 0x80;
    unsigned sessions = 0;
    const std::string child = "current-binding-fixture";
    Fixture() {
        char pattern[] = "/tmp/moguet-current-binding-XXXXXX";
        const char* created = mkdtemp(pattern);
        require(created, "mkdtemp failed");
        root = created;
        db = root / "db";
        fs::create_directories(db / "local");
        write(db / "local/ALPM_DB_VERSION", "9\n");
        package("1-1");
        hooks.database_path = db.string();
        hooks.expected_owner = geteuid();
        hooks.statfs = [](int, struct statfs* metadata) {
            *metadata = {};
            metadata->f_type = 0xef53;
            metadata->f_fsid.__val[0] = 0x1234;
            return 0;
        };
        hooks.name_to_handle = [&](int fd, const char* path, struct file_handle* handle, int* mount, int flags) {
            require(fd >= 0 && path[0] == '\0' && flags == AT_EMPTY_PATH, "generation reopened pathname");
            *mount = 123;
            if(handle->handle_bytes == 0) {
                handle->handle_bytes = 1;
                errno = EOVERFLOW;
                return -1;
            }
            handle->handle_type = 1;
            handle->f_handle[0] = generation;
            return 0;
        };
        hooks.event = [&](Event event) { if(event == Event::BeforeSessionOpen) ++sessions; };
        install();
    }
    Fixture(const Fixture&) = delete;
    ~Fixture() {
        allocation::blocked = false;
        set_current_installed_artifact_binding_test_hook(nullptr);
        set_installed_record_observation_test_hooks({});
        std::error_code error;
        fs::remove_all(root, error);
    }
    void install() {
        set_installed_record_observation_test_hooks(hooks);
    }
    void package(const std::string& version) {
        record = db / "local" / (child + "-" + version);
        fs::create_directories(record);
        write(record / "desc", "%NAME%\n" + child + "\n\n%BASE%\nbase\n\n%VERSION%\n" + version + "\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
        write(record / "files", "%FILES%\nusr/share/current-binding\n\n");
        write(record / "mtree", std::string("\x1f\x8b\x08\0raw-mtree", 13));
    }
    PackageChildIdentity expected(const std::string& base = "base", const std::string& name = "current-binding-fixture",
                                  const std::string& remote = "https://aur.archlinux.org/base.git") const {
        return PackageChildIdentity::make(PackageBaseIdentity::make(
                                              PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote(remote)), base),
                                          name);
    }
    CurrentInstalledArtifactBindingObservation observe() {
        return observe_current_installed_artifact_binding(expected());
    }
};
const CurrentInstalledArtifactBindingObserved& observed(const CurrentInstalledArtifactBindingObservation& value) {
    const auto* result = std::get_if<CurrentInstalledArtifactBindingObserved>(&value);
    require(result, "current observation did not succeed");
    return *result;
}
template <class Cause>
void failure(const CurrentInstalledArtifactBindingObservation& value, Stage stage, Cause cause) {
    const auto* result = std::get_if<CurrentInstalledArtifactBindingFailure>(&value);
    require(result && result->stage == stage && std::get_if<Cause>(&result->cause) && std::get<Cause>(result->cause) == cause,
            "current failure lost stage or original issue");
}
void mismatch(const InstalledArtifactBinding& expected, const InstalledArtifactBinding& current, InstalledArtifactBindingMismatchReason reason) {
    const auto result = compare_installed_artifact_binding(expected, current);
    const auto* detail = std::get_if<InstalledArtifactBindingMismatch>(&result);
    require(detail && detail->reason == reason, "pure installed mismatch reason changed");
}

Fixture* g_fixture = nullptr;
std::string g_fault;
void current_stage(Stage stage) {
    if(stage == Stage::Projection) {
        if(g_fault == "allocation") allocation::blocked = true;
        if(g_fault == "length") throw std::length_error("fixture");
        if(g_fault == "internal") throw std::logic_error("fixture");
        if(g_fault == "malformed") throw std::invalid_argument("fixture");
    }
    if(stage == Stage::WorldRevalidation && (g_fault == "world" || g_fault == "world-failure")) {
        auto& fixture = *g_fixture;
        const auto path = fixture.root / "second-db";
        if(g_fault == "world") {
            fs::create_directories(path / "local");
            write(path / "local/ALPM_DB_VERSION", "9\n");
        }
        fixture.hooks.database_path = path.string();
        fixture.install();
    }
}
void positive_and_drift() {
    for(const std::string mode : {"exact", "generation", "mtree", "database", "version", "architecture", "source-context"}) {
        Fixture fixture;
        const auto first = fixture.observe();
        const auto historical = observed(first).binding();
        require(historical.package() == fixture.expected() && *historical.version().full_version() == "1-1" &&
                    *historical.architecture().value() == "any",
                "semantic projection mismatch");
        require(historical.mtree_digest().value() == xdg_generation_store_raw_contents_sha256(read(fixture.record / "mtree")), "MTREE normalized");
        const auto db_bytes = std::string("desc\0", 5) + read(fixture.record / "desc") + std::string("files\0", 6) + read(fixture.record / "files");
        require(historical.database_record_digest().value() == xdg_generation_store_raw_contents_sha256(db_bytes), "DB digest mismatch");
        require(historical.record_generation().opaque_identity() == "linux-name-to-handle-at-v1|fs=000000000000ef53|fsid=0000123400000000|type=00000001|length=1|handle=80", "generation projection mismatch");
        if(mode == "generation") fixture.generation = 1; // Deliberately not monotonic.
        if(mode == "mtree") write(fixture.record / "mtree", "changed raw MTREE");
        if(mode == "database") write(fixture.record / "desc", read(fixture.record / "desc") + "\n");
        if(mode == "architecture") {
            auto bytes = read(fixture.record / "desc");
            bytes.replace(bytes.find("any"), 3, "x86_64");
            write(fixture.record / "desc", bytes);
        }
        if(mode == "version") {
            fs::rename(fixture.record, fixture.root / "old-record");
            fixture.package("2-1");
        }
        const auto current = mode == "source-context"
                                 ? observe_current_installed_artifact_binding(fixture.expected("base", fixture.child, "https://aur.archlinux.org/other.git"))
                                 : fixture.observe();
        const auto& binding = observed(current).binding();
        require(fixture.sessions == 2, "current observer reused an ALPM session");
        using Reason = InstalledArtifactBindingMismatchReason;
        if(mode == "exact") {
            require(std::holds_alternative<InstalledArtifactBindingMatch>(compare_installed_artifact_binding(historical, binding)), "exact current comparison failed");
            auto copy = observed(current);
            require(copy.binding() == binding && fixture.sessions == 2, "copy refreshed observation");
            bool denied = false;
            allocation::blocked = true;
            try {
                copy = observed(first);
            } catch(const std::bad_alloc&) {
                denied = true;
            }
            allocation::blocked = false;
            require(denied && copy.binding() == binding && copy.world() == observed(current).world(), "failed copy changed snapshot pair");
            allocation::failures = 0;
        } else {
            const auto reason = mode == "generation" ? Reason::RecordGenerationMismatch : mode == "mtree"      ? Reason::MtreeMismatch
                                                                                      : mode == "database"     ? Reason::DatabaseRecordMismatch
                                                                                      : mode == "version"      ? Reason::VersionMismatch
                                                                                      : mode == "architecture" ? Reason::ArchitectureMismatch
                                                                                                               : Reason::PackageIdentityMismatch;
            mismatch(historical, binding, reason);
        }
        std::cout << "S7A current " << mode << " PASS\n";
    }
}
void negative_matrix() {
    for(const std::string mode : {"absent", "base", "load", "malformed-db", "record-replaced", "world-during-record", "unsupported", "read", "low-resource",
                                  "world", "absent-world", "world-failure", "allocation", "length", "internal", "malformed"}) {
        Fixture fixture;
        g_fixture = &fixture;
        g_fault = mode == "absent-world" ? "world" : mode;
        if(mode == "load") fixture.hooks.fail_database_load = true;
        if(mode == "malformed-db") write(fixture.record / "desc", read(fixture.record / "desc") + "%BASE%\nconflict\n\n");
        if(mode == "unsupported") fixture.hooks.name_to_handle = [](int, const char*, struct file_handle*, int*, int) { errno = EOPNOTSUPP; return -1; };
        if(mode == "read") fixture.hooks.pread = [](int, void*, std::size_t, off_t) -> ssize_t { errno = EIO; return -1; };
        if(mode == "low-resource") fixture.hooks.pread = [](int, void*, std::size_t, off_t) -> ssize_t { throw std::bad_alloc(); };
        bool replaced = false;
        if(mode == "record-replaced" || mode == "world-during-record") fixture.hooks.event = [&](Event event) {
            if(event != Event::BeforeFinalReproof || replaced) return;
            replaced = true;
            if(mode == "world-during-record")
                fs::rename(fixture.db, fixture.root / "old-db");
            else {
                const auto bytes = read(fixture.record / "files");
                write(fixture.root / "replacement", bytes);
                struct stat before{}, after{};
                require(stat((fixture.record / "files").c_str(), &before) == 0, "stat failed");
                fs::rename(fixture.root / "replacement", fixture.record / "files");
                require(stat((fixture.record / "files").c_str(), &after) == 0 && before.st_ino != after.st_ino && read(fixture.record / "files") == bytes,
                        "replacement must preserve bytes and change inode");
            }
        };
        fixture.install();
        set_current_installed_artifact_binding_test_hook(current_stage);
        const auto value = observe_current_installed_artifact_binding(fixture.expected(mode == "base" ? "other" : "base",
                                                                                       mode == "absent" || mode == "absent-world" ? "missing-child" : fixture.child));
        allocation::blocked = false;
        if(mode == "absent")
            require(std::holds_alternative<CurrentInstalledArtifactBindingAbsent>(value), "absence flattened");
        else if(mode == "base")
            failure(value, Stage::Projection, Issue::MetadataMismatch);
        else if(mode == "world" || mode == "absent-world")
            failure(value, Stage::WorldRevalidation, Issue::DatabaseWorldMismatch);
        else if(mode == "world-failure")
            failure(value, Stage::WorldRevalidation, Issue::UnsupportedDatabaseWorld);
        else if(mode == "allocation" || mode == "length")
            failure(value, Stage::Projection, Issue::ResourceFailure);
        else if(mode == "internal")
            failure(value, Stage::Projection, CurrentInstalledArtifactBindingIssue::InternalFailure);
        else if(mode == "malformed")
            failure(value, Stage::Projection, CurrentInstalledArtifactBindingIssue::MalformedBinding);
        else {
            const auto issue = mode == "load" ? Issue::DatabaseLoadFailure : mode == "malformed-db" ? Issue::MalformedMetadata
                                                                         : mode == "unsupported"    ? Issue::UnsupportedGeneration
                                                                         : mode == "read"           ? Issue::ReadFailure
                                                                         : mode == "low-resource"   ? Issue::ResourceFailure
                                                                                                    : Issue::RecordChanged;
            failure(value, Stage::RecordObservation, issue);
        }
        if(mode == "record-replaced" || mode == "world-during-record") require(replaced, "race did not execute");
        if(mode == "allocation") require(allocation::failures == 1, "resource return allocated again");
        std::cout << "S7A current " << mode << " PASS\n";
    }
}
void invalid_expected_identity() {
    Fixture fixture;
    const auto expected = PackageChildIdentity::make(PackageBaseIdentity::make(
                                                         PackageSourceIdentity::aur(SourceLocationIdentity::unknown(SourceLocationKind::GitRemote)), "base"),
                                                     fixture.child);
    failure(observe_current_installed_artifact_binding(expected), Stage::ExpectedIdentity,
            CurrentInstalledArtifactBindingIssue::InvalidExpectedIdentity);
    require(fixture.sessions == 0, "invalid source context reached DB observation");
    std::cout << "S7A current invalid-expected-identity PASS\n";
}
void read_only_boundary() {
    Fixture fixture;
    std::vector<std::pair<fs::path, std::string>> before;
    for(const auto& entry : fs::recursive_directory_iterator(fixture.root))
        before.emplace_back(entry.path(), entry.is_regular_file() ? read(entry.path()) : "directory");
    require(std::holds_alternative<CurrentInstalledArtifactBindingObserved>(fixture.observe()), "read-only observation failed");
    std::size_t count = 0;
    for(auto entry = fs::recursive_directory_iterator(fixture.root); entry != fs::recursive_directory_iterator(); ++entry)
        ++count;
    require(count == before.size(), "observer created namespace entries");
    for(const auto& [path, bytes] : before)
        require(bytes == (fs::is_regular_file(path) ? read(path) : "directory"), "observer mutated DB");
    fixture.hooks.database_path = (fixture.root / "missing-db").string();
    fixture.install();
    failure(fixture.observe(), Stage::WorldResolution, Issue::UnsupportedDatabaseWorld);
    require(!fs::exists(fixture.root / "missing-db"), "missing DB was created");
    std::cout << "S7A current read-no-create PASS\n";
}
} // namespace
int main() {
    try {
        positive_and_drift();
        negative_matrix();
        read_only_boundary();
        invalid_expected_identity();
    } catch(const std::exception& error) {
        allocation::blocked = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
