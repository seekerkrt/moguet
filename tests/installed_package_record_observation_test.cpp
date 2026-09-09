#include "installed_package_record_observation.hpp"
#include "package_metadata.hpp"
#include "xdg_generation_store.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Issue = InstalledRecordObservationIssue;
using Event = InstalledRecordObservationTestEvent;

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(!output.fail(), "fixture write failed");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

class Fixture final {
public:
    fs::path root, db, record;
    InstalledRecordObservationTestHooks hooks;
    unsigned generation = 1;
    unsigned generation_calls = 0;
    unsigned fresh_sessions = 0;
    const std::string name = "moguet-installed-observer-fixture";

    Fixture() {
        std::array<char, 64> pattern{};
        std::strcpy(pattern.data(), "/tmp/moguet-installed-observer-XXXXXX");
        const auto* created = mkdtemp(pattern.data());
        require(created, "mkdtemp failed");
        root = created;
        db = root / "db";
        fs::create_directories(db / "local");
        write_file(db / "local/ALPM_DB_VERSION", "9\n");
        create_package("1-1");
        hooks.database_path = db.string();
        hooks.expected_owner = geteuid();
        hooks.statfs = [](int, struct statfs* result) {
            *result = {};
            result->f_type = 0xef53;
            result->f_fsid.__val[0] = 0x12345678;
            result->f_fsid.__val[1] = static_cast<int>(0x90abcdefU);
            return 0;
        };
        hooks.name_to_handle = [&](int descriptor, const char* path, struct file_handle* handle, int* mount, int flags) {
            require(descriptor >= 0 && path[0] == '\0' && flags == AT_EMPTY_PATH, "generation reopened a path");
            ++generation_calls;
            *mount = 100 + static_cast<int>(generation_calls); // Must never affect persistent equality.
            if(handle->handle_bytes == 0) {
                handle->handle_bytes = 4;
                errno = EOVERFLOW;
                return -1;
            }
            require(handle->handle_bytes == 4, "sized allocation length was lost");
            handle->handle_type = 1;
            for(unsigned index = 0; index < 4; ++index)
                handle->f_handle[index] = static_cast<unsigned char>(generation + index);
            return 0;
        };
        hooks.event = [&](Event phase) { if(phase == Event::BeforeSessionOpen) ++fresh_sessions; };
        install_hooks();
    }
    Fixture(const Fixture&) = delete;
    ~Fixture() {
        set_installed_record_observation_test_hooks({});
        std::error_code error;
        fs::remove_all(root, error);
    }
    void install_hooks() {
        set_installed_record_observation_test_hooks(hooks);
    }
    void create_package(const std::string& version) {
        record = db / "local" / (name + "-" + version);
        fs::create_directories(record);
        write_file(record / "desc", "%NAME%\n" + name + "\n\n%BASE%\nbase\n\n%VERSION%\n" + version + "\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
        write_file(record / "files", "%FILES%\nusr/share/moguet-fixture\n\n");
        // A binary raw MTREE preimage. This fixture tests descriptor/byte
        // authority, not a package-manager transaction or MTREE parser.
        write_file(record / "mtree", std::string("\x1f\x8b\x08\0fixture-mtree\0", 18));
    }
    InstalledDatabaseWorld world() {
        auto result = resolve_trusted_installed_database_world();
        require(std::holds_alternative<InstalledDatabaseWorld>(result), "fixture world was not resolved");
        return std::get<InstalledDatabaseWorld>(result);
    }
    InstalledPackageRecordObservation observe() {
        return observe_installed_package_record(world(), name);
    }
    void replace_child(const std::string& child) {
        const auto replacement = root / "replacement";
        write_file(replacement, read_file(record / child));
        fs::rename(replacement, record / child);
    }
};

void expect_issue(const InstalledPackageRecordObservation& result, Issue expected, const std::string& label) {
    const auto* issue = std::get_if<Issue>(&result);
    require(issue && *issue == expected, label + " did not produce its typed failure");
}

void fresh_and_raw_matrix() {
    Fixture fixture;
    auto old = PackageMetadataSession::open(PacmanDatabasePaths{"/", fixture.db});
    require(std::get<InstalledPackageMetadata>(old.query_installed_package(fixture.name)).version == "1-1", "old session fixture failed");
    const auto first = std::get<InstalledPackageRecordSnapshot>(fixture.observe());
    require(first.architecture == "any" && first.package_base == "base" && first.full_version == "1-1", "semantic identity changed");
    const auto raw_desc = read_file(fixture.record / "desc"), raw_files = read_file(fixture.record / "files");
    std::string preimage("desc\0", 5);
    preimage += raw_desc;
    preimage.append("files\0", 6).append(raw_files);
    require(first.raw_database_sha256 == xdg_generation_store_raw_contents_sha256(preimage), "desc/files preimage was normalized");
    require(first.raw_mtree_sha256 == xdg_generation_store_raw_contents_sha256(read_file(fixture.record / "mtree")), "binary MTREE was normalized");
    require(std::get<InstalledPackageRecordSnapshot>(parse_installed_package_record_observation(serialize_installed_package_record_observation(first))) == first, "snapshot protocol roundtrip failed");
    auto world = fixture.world();
    require(std::get<InstalledDatabaseWorld>(parse_installed_database_world(serialize_installed_database_world(world))) == world, "world protocol roundtrip failed");
    fs::rename(fixture.record, fixture.root / "old-record");
    fixture.create_package("2-1");
    ++fixture.generation;
    const auto fresh = std::get<InstalledPackageRecordSnapshot>(fixture.observe());
    require(fresh.full_version == "2-1" && fresh.record_generation != first.record_generation && fixture.fresh_sessions == 2, "fresh handle did not see upgraded record");
    require(std::get<InstalledPackageMetadata>(old.query_installed_package(fixture.name)).version == "1-1", "stale-session positive control unexpectedly refreshed");
    require(std::holds_alternative<InstalledPackageRecordAbsent>(observe_installed_package_record(world, "missing-package")), "missing package became database failure");
    fixture.hooks.fail_database_load = true;
    fixture.install_hooks();
    expect_issue(observe_installed_package_record(world, "missing-package"), Issue::DatabaseLoadFailure, "load failure vs absence");
}

void malformed_and_io_matrix() {
    for(const auto& scenario : {"desc-nul", "files-nul", "missing-base", "duplicate-name", "duplicate-record", "wrong-record-name", "wrong-core-name", "empty-mtree",
                                "oversized-desc", "oversized-files", "oversized-mtree", "short-read", "eio", "missing-mtree", "corrupt-db"}) {
        Fixture fixture;
        const std::string mode(scenario);
        Issue expected = Issue::MalformedMetadata;
        if(mode == "desc-nul" || mode == "files-nul") {
            const auto leaf = mode == "desc-nul" ? "desc" : "files";
            write_file(fixture.record / leaf, read_file(fixture.record / leaf) + std::string(1, '\0'));
        } else if(mode == "missing-base") {
            auto bytes = read_file(fixture.record / "desc");
            bytes.erase(bytes.find("%BASE%\nbase\n\n"), 13);
            write_file(fixture.record / "desc", bytes);
        } else if(mode == "duplicate-name") {
            write_file(fixture.record / "desc", read_file(fixture.record / "desc") + "%NAME%\n" + fixture.name + "\n\n");
        } else if(mode == "duplicate-record") {
            fs::copy(fixture.record, fixture.db / "local" / (fixture.name + "-other"), fs::copy_options::recursive);
        } else if(mode == "wrong-record-name") {
            fs::rename(fixture.record, fixture.db / "local" / (fixture.name + "-other"));
        } else if(mode == "wrong-core-name") {
            auto bytes = read_file(fixture.record / "desc");
            bytes.replace(bytes.find(fixture.name), fixture.name.size(), "wrong-name");
            write_file(fixture.record / "desc", bytes);
        } else if(mode == "empty-mtree") {
            write_file(fixture.record / "mtree", "");
        } else if(mode.starts_with("oversized-")) {
            const auto leaf = mode.substr(10);
            fs::resize_file(fixture.record / leaf, (leaf == "mtree" ? INSTALLED_RECORD_MAXIMUM_MTREE_BYTES : INSTALLED_RECORD_MAXIMUM_METADATA_BYTES) + 1);
            expected = Issue::MetadataTooLarge;
        } else if(mode == "short-read" || mode == "eio") {
            fixture.hooks.pread = [mode](int, void*, std::size_t, off_t) -> ssize_t { errno = EIO; return mode == "eio" ? -1 : 0; };
            expected = Issue::ReadFailure;
        } else if(mode == "missing-mtree") {
            fs::remove(fixture.record / "mtree");
            expected = Issue::UnsafeRecord;
        } else if(mode == "corrupt-db") {
            write_file(fixture.db / "local/ALPM_DB_VERSION", "unsupported\n");
            expected = Issue::DatabaseLoadFailure;
        }
        fixture.install_hooks();
        expect_issue(fixture.observe(), expected, mode);
    }
    for(const auto& leaf : {"desc", "files", "mtree"}) {
        Fixture fixture;
        const auto original = std::get<InstalledPackageRecordSnapshot>(fixture.observe());
        auto bytes = read_file(fixture.record / leaf);
        if(std::string(leaf) == "mtree")
            bytes[3] = '\x01';
        else
            bytes.push_back('\n');
        write_file(fixture.record / leaf, bytes);
        const auto changed = std::get<InstalledPackageRecordSnapshot>(fixture.observe());
        require(std::string(leaf) == "mtree" ? changed.raw_mtree_sha256 != original.raw_mtree_sha256
                                             : changed.raw_database_sha256 != original.raw_database_sha256,
                "raw bytes were normalized or ignored");
    }
}

void descriptor_matrix() {
    for(const auto phase : {Event::AfterRecordSelection, Event::AfterRecordOpen, Event::BeforeRawRead, Event::AfterRawRead, Event::BeforeFinalReproof}) {
        for(const auto& object : {"record", "desc", "files", "mtree", "parent"}) {
            Fixture fixture;
            bool changed = false;
            fixture.hooks.event = [&](Event current) {
                if(changed || current != phase) return;
                changed = true;
                if(std::string(object) == "record") {
                    fs::rename(fixture.record, fixture.root / "old-record");
                    fixture.create_package("1-1");
                } else if(std::string(object) == "parent") {
                    fs::rename(fixture.db, fixture.root / "old-db");
                    fs::create_directories(fixture.db / "local");
                } else
                    fixture.replace_child(object);
            };
            fixture.install_hooks();
            const auto observed = fixture.observe();
            require(changed && std::holds_alternative<Issue>(observed),
                    "descriptor replacement was accepted: phase=" + std::to_string(static_cast<int>(phase)) + " object=" + object);
        }
    }
    for(const auto& object : {"desc", "files", "mtree"}) {
        for(const auto& mode : {"symlink", "hardlink", "writable"}) {
            Fixture fixture;
            const auto child = fixture.record / object;
            if(std::string(mode) == "symlink") {
                fs::rename(child, fixture.root / "saved");
                fs::create_symlink(fixture.root / "saved", child);
            } else if(std::string(mode) == "hardlink")
                fs::create_hard_link(child, fixture.root / "extra-link");
            else
                fs::permissions(child, fs::perms::others_write, fs::perm_options::add);
            expect_issue(fixture.observe(), Issue::UnsafeRecord, std::string(object) + "/" + mode);
        }
    }
    Fixture fixture;
    auto world = fixture.world();
    world.root_directory = "/wrong-root";
    expect_issue(observe_installed_package_record(world, fixture.name), Issue::UnsupportedDatabaseWorld, "wrong RootDir");
    world = fixture.world();
    world.descriptor_identity += "changed";
    expect_issue(observe_installed_package_record(world, fixture.name), Issue::DatabaseWorldMismatch, "wrong DB world");
}

void generation_matrix() {
    Fixture fixture;
    const int descriptor = open(fixture.record.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(descriptor >= 0, "generation descriptor failed");
    const auto observe = [&] { return observe_installed_record_generation_for_test(descriptor); };
    const auto original = std::get<std::string>(observe());
    require(original == "linux-name-to-handle-at-v1|fs=000000000000ef53|fsid=1234567890abcdef|type=00000001|length=4|handle=01020304", "generation encoding differs from independent oracle");
    require(std::get<std::string>(observe()) == original, "transient mount ID entered persistent equality");
    require(fixture.generation_calls == 4, "size=0/EOVERFLOW/retry protocol was not used");
    const auto normal = fixture.hooks;
    for(const auto& scenario : {"unsupported", "permission", "zero", "oversize", "changed-length", "zero-type", "retry-overflow", "magic", "fsid", "type", "opaque"}) {
        fixture.hooks = normal;
        const std::string mode(scenario);
        auto real_handle = normal.name_to_handle;
        fixture.hooks.name_to_handle = [mode, real_handle](int fd, const char* path, struct file_handle* handle, int* mount, int flags) {
            const bool sizing = handle->handle_bytes == 0;
            if(mode == "unsupported" || mode == "permission") {
                errno = mode == "unsupported" ? EOPNOTSUPP : EPERM;
                return -1;
            }
            const int result = real_handle(fd, path, handle, mount, flags);
            if(sizing && mode == "zero") handle->handle_bytes = 0;
            if(sizing && mode == "oversize") handle->handle_bytes = 4096;
            if(!sizing && mode == "changed-length") --handle->handle_bytes;
            if(!sizing && mode == "zero-type") handle->handle_type = 0;
            if(!sizing && mode == "retry-overflow") {
                errno = EOVERFLOW;
                return -1;
            }
            if(!sizing && mode == "type") ++handle->handle_type;
            if(!sizing && mode == "opaque") ++handle->f_handle[0];
            return result;
        };
        auto real_statfs = normal.statfs;
        fixture.hooks.statfs = [mode, real_statfs](int fd, struct statfs* metadata) {
            const auto result = real_statfs(fd, metadata);
            if(mode == "magic") ++metadata->f_type;
            if(mode == "fsid") ++metadata->f_fsid.__val[0];
            return result;
        };
        fixture.install_hooks();
        const auto result = observe();
        if(mode == "magic" || mode == "fsid" || mode == "type" || mode == "opaque")
            require(std::holds_alternative<std::string>(result) && std::get<std::string>(result) != original, "generation identity component was ignored");
        else
            require(std::holds_alternative<Issue>(result) && std::get<Issue>(result) == Issue::UnsupportedGeneration, "unsupported generation used a fallback");
    }
    static_cast<void>(close(descriptor));
}

void fixed_configuration_ignores_path() {
    Fixture fixture;
    const auto executable = fixture.root / "pacman-conf";
    write_file(executable, "#!/bin/sh\nprintf spoof > \"$0.invoked\"\nprintf 'RootDir = /wrong\\nDBPath = /wrong\\n'\n");
    fs::permissions(executable, fs::perms::owner_all, fs::perm_options::replace);
    set_installed_record_observation_test_hooks({});
    const auto before = resolve_trusted_installed_database_world();
    struct RestorePath {
        std::optional<std::string> value;
        ~RestorePath() {
            if(value)
                static_cast<void>(setenv("PATH", value->c_str(), 1));
            else
                static_cast<void>(unsetenv("PATH"));
        }
    } saved;
    if(const auto* value = std::getenv("PATH")) saved.value = value;
    require(setenv("PATH", fixture.root.c_str(), 1) == 0, "PATH fixture setup failed");
    const auto after = resolve_trusted_installed_database_world();
    require(before == after && !fs::exists(executable.string() + ".invoked"), "PATH spoof became database-world authority");
}

void blocker_first_child_capture(const std::string& child) {
    Fixture fixture;
    fixture.hooks.ignore_directory_content_changes = true;
    const auto original = read_file(fixture.record / child);
    std::array<int, 3> retained{-1, -1, -1};
    std::array<unsigned, 3> opens{};
    constexpr std::array<std::string_view, 3> children{"desc", "files", "mtree"};
    bool all_captured_at_admission = false;
    bool replaced = false;
    ino_t before = 0, after = 0;
    fixture.hooks.child_opened = [&](std::string_view name, int fd) {
        for(std::size_t i = 0; i < children.size(); ++i) {
            if(name == children[i]) {
                retained[i] = fd;
                ++opens[i];
            }
        }
    };
    fixture.hooks.event = [&](Event phase) {
        if(phase != Event::AfterRawDescAdmission || replaced) return;
        // This event follows the actual core parse/name match. The separate
        // open callback records successful syscalls, not a movable phase label.
        all_captured_at_admission = std::all_of(opens.begin(), opens.end(), [](unsigned count) { return count == 1; });
        if(all_captured_at_admission) {
            for(int fd : retained) {
                struct stat st{};
                require(fstat(fd, &st) == 0, "child FD closed before selection");
            }
        }
        struct stat st{};
        require(stat((fixture.record / child).c_str(), &st) == 0, "original child stat failed");
        before = st.st_ino;
        fixture.replace_child(child);
        require(stat((fixture.record / child).c_str(), &st) == 0, "replacement stat failed");
        after = st.st_ino;
        replaced = true;
    };
    fixture.install_hooks();
    const auto observed = fixture.observe();
    require(replaced && before != after && read_file(fixture.record / child) == original && fixture.generation == 1,
            "replacement fixture lost same bytes/new inode/fixed record generation");
    expect_issue(observed, Issue::RecordChanged, "F-S5B-01 " + child);
    require(all_captured_at_admission, "first required child open occurred after raw desc selection");
    require(std::all_of(opens.begin(), opens.end(), [](unsigned count) { return count == 1; }), "required child was reopened");
}

void blocker_empty_sections() {
    {
        Fixture fixture;
        write_file(fixture.record / "desc", read_file(fixture.record / "desc") + "%DESC%\n\n%UNRECOGNIZED%\n\n%XDATA%\nkey=value\nempty=\n\n");
        require(std::holds_alternative<InstalledPackageRecordSnapshot>(fixture.observe()), "empty noncore sections became invalid");
    }
    unsigned rejected = 0;
    for(const std::string key : {"NAME", "BASE", "VERSION", "ARCH"}) {
        for(bool conflict : {false, true}) {
            Fixture fixture;
            const auto value = conflict ? "conflicting-value" : key == "NAME"  ? fixture.name
                                                            : key == "BASE"    ? "base"
                                                            : key == "VERSION" ? "1-1"
                                                                               : "any";
            write_file(fixture.record / "desc", read_file(fixture.record / "desc") + "%DESC%\n\n%" + key + "%\n" + value + "\n\n");
            const auto observed = fixture.observe();
            if(std::holds_alternative<Issue>(observed)) ++rejected;
            std::cout << "F-S5B-02 " << key << (conflict ? " conflict " : " duplicate ")
                      << (std::holds_alternative<Issue>(observed) ? "REJECT" : "ACCEPTED") << '\n';
        }
    }
    require(rejected == 8, "empty noncore value hid a duplicate/conflicting core field");
    for(const std::string mode : {"empty-core", "multiline-core", "extra-delimiter", "unterminated"}) {
        Fixture fixture;
        auto bytes = read_file(fixture.record / "desc");
        if(mode == "empty-core" || mode == "multiline-core")
            bytes.replace(bytes.find("%BASE%\nbase\n\n"), 13, mode == "empty-core" ? "%BASE%\n\n" : "%BASE%\nbase\nextra\n\n");
        else
            bytes += mode == "extra-delimiter" ? "%%\nvalue\n\n" : "%EXTRA%\nvalue\n";
        write_file(fixture.record / "desc", bytes);
        expect_issue(fixture.observe(), Issue::MalformedMetadata, "core/section boundary " + mode);
    }
}

void blocker_raw_admission(const std::string& mode) {
    Fixture fixture;
    if(mode == "xdata")
        write_file(fixture.record / "desc", read_file(fixture.record / "desc") + "%XDATA%\nnot-key-value\n\n");
    else
        fs::resize_file(fixture.record / "desc", INSTALLED_RECORD_MAXIMUM_METADATA_BYTES + 1);
    unsigned semantic_loads = 0;
    fixture.hooks.event = [&](Event phase) { if(phase == Event::BeforeSemanticLoad) ++semantic_loads; };
    fixture.install_hooks();
    const auto observed = fixture.observe();
    expect_issue(observed, mode == "xdata" ? Issue::MalformedMetadata : Issue::MetadataTooLarge, "F-S5B-03 raw " + mode);
    require(semantic_loads == 0, "broken/oversized desc reached ALPM lazy read");
}

void blocker_late_lazy_failure() {
    Fixture fixture;
    bool raw_admitted = false, late_failure = false, partial_values = false;
    fixture.hooks.event = [&](Event phase) {
        if(phase == Event::AfterRawDescAdmission) raw_admitted = true;
        if(phase == Event::BeforeSemanticLoad) {
            // Real libalpm loads the four valid core values, then encounters
            // invalid XDATA. LAZY_LOAD does not return that failure to getters.
            write_file(fixture.record / "desc", read_file(fixture.record / "desc") + "%XDATA%\nnot-key-value\n\n");
            late_failure = true;
        }
        if(phase == Event::AfterSemanticLoad) partial_values = true;
    };
    fixture.install_hooks();
    const auto observed = fixture.observe();
    expect_issue(observed, Issue::DatabaseLoadFailure, "F-S5B-03 late lazy load");
    require(raw_admitted && late_failure && partial_values, "late lazy-load fixture did not reach its actual boundary");
}

void blocker_ambiguous_lazy_completion() {
    Fixture fixture;
    write_file(fixture.record / "files", "%FILES%\n\n");
    expect_issue(fixture.observe(), Issue::DatabaseLoadFailure, "empty FILES cannot attest DESC completion");
}

void blocker_regressions() {
    unsigned failures = 0;
    const auto check = [&](const std::string& label, const auto& run) {
        try {
            run();
            std::cout << label << " PASS\n";
        } catch(const std::exception& error) {
            ++failures;
            std::cerr << label << " FAIL: " << error.what() << '\n';
        }
    };
    check("F-S5B-01 first files open", [] { blocker_first_child_capture("files"); });
    check("F-S5B-01 first mtree open", [] { blocker_first_child_capture("mtree"); });
    check("F-S5B-01 retained desc control", [] { blocker_first_child_capture("desc"); });
    check("F-S5B-02 section admission", blocker_empty_sections);
    check("F-S5B-03 malformed XDATA admission", [] { blocker_raw_admission("xdata"); });
    check("F-S5B-03 oversized desc admission", [] { blocker_raw_admission("oversize"); });
    check("F-S5B-03 late lazy failure", blocker_late_lazy_failure);
    check("F-S5B-03 ambiguous completion", blocker_ambiguous_lazy_completion);
    require(failures == 0, "S5-B blocker regressions failed: " + std::to_string(failures));
}

} // namespace

int main() {
    try {
        blocker_regressions();
        fresh_and_raw_matrix();
        malformed_and_io_matrix();
        descriptor_matrix();
        generation_matrix();
        fixed_configuration_ignores_path();
        std::cout << "installed-package-record-observation: fresh/raw/descriptor/generation matrix PASS\n";
    } catch(const std::exception& error) {
        std::cerr << "installed-package-record-observation: " << error.what() << '\n';
        return 1;
    }
}
