#include "artifact_workspace.hpp"
#include "package_base_artifact_install_executor.hpp"
#include "process.hpp"
#include "source_artifact_install_trusted_helper_state.hpp"
#include "source_artifact_install_trusted_protocol.hpp"
#include "source_artifact_install_trusted_transport.hpp"
#include "source_package_identity.hpp"
#include "trusted_alpm_receipt_helper_state.hpp"
#include "trusted_alpm_receipt_protocol.hpp"
#include "trusted_alpm_receipt_transport.hpp"
#include "trusted_cache_test_support.hpp"
#include "xdg_generation_store.hpp"
#include <alpm.h>
#include <cstdio>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <map>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <linux/memfd.h>

static_assert(
    !std::is_convertible_v<
        SourceArtifactInstallTrustedExecutionResult,
        SourceArtifactInstallReceiptObservation>);
static_assert(
    !std::is_convertible_v<
        SourceArtifactInstallTrustedExecutionResult,
        SourceArtifactInstallCausalEvidence>);
static_assert(
    !std::is_convertible_v<
        TrustedAlpmReceiptCaptureResult,
        SourceArtifactInstallTrustedExecutionResult>);

namespace fs = std::filesystem;

namespace {

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Function>
void expect_failure(Function&& function, const std::string& message) {
    bool failed = false;
    try {
        function();
    } catch(const std::exception&) {
        failed = true;
    }
    expect(failed, message);
}

std::string transaction_token(char character) {
    return std::string(64, character);
}

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(std::string prefix) {
        std::vector<char> path_template;
        const std::string template_text =
            (fs::temp_directory_path() / (prefix + "-XXXXXX")).string();
        path_template.assign(template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created = mkdtemp(path_template.data());
        if(created == nullptr) {
            throw std::runtime_error("failed to create temporary directory");
        }
        path_ = created;
        fs::permissions(
            path_, fs::perms::owner_all, fs::perm_options::replace);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

class ActualArchiveFixture final {
public:
    ActualArchiveFixture() : directory_("moguet-source-receipt-archive") {
    }

    fs::path create_archive(
        const std::string& package_name,
        const std::string& full_version,
        const std::string& package_base,
        const std::string& architecture, bool uncompressed = false) {
        const fs::path root = directory_.path() / package_name;
        fs::create_directory(root);
        std::ofstream pkginfo(root / ".PKGINFO");
        if(!pkginfo) throw std::runtime_error("failed to create .PKGINFO");
        pkginfo << "pkgname = " << package_name << '\n';
        pkginfo << "pkgbase = " << package_base << '\n';
        pkginfo << "pkgver = " << full_version << '\n';
        pkginfo << "pkgdesc = Moguet source artifact receipt test\n";
        pkginfo << "url = https://example.invalid/moguet\n";
        pkginfo << "builddate = 1\n";
        pkginfo << "packager = Moguet tests\n";
        pkginfo << "size = 0\n";
        pkginfo << "arch = " << architecture << '\n';
        pkginfo << "license = GPL\n";
        pkginfo.close();
        if(!pkginfo) throw std::runtime_error("failed to finish .PKGINFO");

        {
            constexpr char mtree[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x53\xce\x2d\x29\x4a\x4d\xe5\xd2\xd3\x2f\x48\xac\xcc\xc9\x4f\x4c\x51\x28\xa9\x2c\x48\xb5\x4d\xcb\xcc\x49\x55\x28\xce\xac\x4a\xb5\x35\xe3\x02\x00\x0d\xcc\xc5\xf8\x22\x00\x00\x00";
            std::ofstream output(root / ".MTREE", std::ios::binary);
            output.write(mtree, sizeof(mtree) - 1);
            std::ofstream(root / "payload", std::ios::binary) << "hello\n";
        }
        const fs::path archive =
            directory_.path() /
            (package_name + "-" + full_version + "-" + architecture +
             (uncompressed ? ".pkg.tar" : ".pkg.tar.zst"));
        const pid_t child = fork();
        if(child < 0) throw std::runtime_error("failed to fork bsdtar");
        if(child == 0) {
            if(uncompressed) {
                execl("/usr/bin/bsdtar", "bsdtar", "--format=ustar", "-cf",
                      archive.c_str(), "-C", root.c_str(), ".PKGINFO", ".MTREE", "payload",
                      static_cast<char*>(nullptr));
                _exit(127);
            }
            execl(
                "/usr/bin/bsdtar", "bsdtar", "--zstd", "-cf",
                archive.c_str(), "-C", root.c_str(), ".PKGINFO", ".MTREE", "payload",
                static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        if(waitpid(child, &status, 0) != child ||
           !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            throw std::runtime_error("failed to create package archive");
        }
        return archive;
    }

private:
    TemporaryDirectory directory_;
};

class OwnedDescriptor final {
public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }
    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;
    OwnedDescriptor(OwnedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }
    ~OwnedDescriptor() {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }
    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

void write_all(int descriptor, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while(offset < size) {
        const ssize_t written = write(descriptor, data + offset, size - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written == -1 && errno == EINTR) continue;
        throw std::runtime_error("failed to write fixture descriptor");
    }
}

OwnedDescriptor sealed_input_from_files(
    const std::vector<fs::path>& files) {
#ifdef SYS_memfd_create
    const long raw_descriptor = syscall(
        SYS_memfd_create, "moguet-source-receipt-test",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if(raw_descriptor < 0) throw std::runtime_error("memfd_create failed");
    OwnedDescriptor descriptor(static_cast<int>(raw_descriptor));
    std::array<char, 64U * 1024U> buffer{};
    for(const fs::path& file : files) {
        std::ifstream input(file, std::ios::binary);
        if(!input) throw std::runtime_error("failed to read archive fixture");
        while(input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if(count > 0) {
                write_all(
                    descriptor.get(), buffer.data(),
                    static_cast<std::size_t>(count));
            }
        }
        if(!input.eof()) throw std::runtime_error("archive fixture read failed");
    }
    constexpr int seals =
        F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if(fcntl(descriptor.get(), F_ADD_SEALS, seals) == -1 ||
       lseek(descriptor.get(), 0, SEEK_SET) == -1) {
        throw std::runtime_error("failed to seal archive fixture");
    }
    return descriptor;
#else
    static_cast<void>(files);
    throw std::runtime_error("memfd_create is unavailable");
#endif
}

std::uint64_t fixture_file_size(const fs::path& path) {
    return static_cast<std::uint64_t>(fs::file_size(path));
}

std::string fixture_sha256(const fs::path& path) {
    OwnedDescriptor descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    expect(descriptor.get() >= 0, "failed to open digest fixture");
    return xdg_generation_store_file_descriptor_sha256(descriptor.get(), fixture_file_size(path),
                                                       SOURCE_ARTIFACT_INSTALL_MAXIMUM_TRANSACTION_BYTES);
}

SourceArtifactInstallRootPrepareRequest root_request(
    const std::string& token,
    const fs::path& archive,
    const std::string& package_name = "moguet-source-transport-test",
    const std::string& full_version = "1-1",
    const std::string& package_base = "moguet-source-transport-base",
    const std::string& architecture = "any") {
    return SourceArtifactInstallRootPrepareRequest{
        token,
        package_base,
        SourceArtifactInstallTrustedDirective::AsDependency,
        false,
        true,
        {{0,
          package_name,
          full_version,
          package_base,
          architecture,
          fixture_file_size(archive),
          fs::exists(archive.string() + ".sig") ? fixture_file_size(archive.string() + ".sig") : 0,
          fixture_sha256(archive),
          fs::exists(archive.string() + ".sig") ? fixture_sha256(archive.string() + ".sig") : "-"}}};
}

SourceArtifactInstallTrustedStateStore open_source_store(
    const TemporaryDirectory& directory) {
    const int descriptor = open(
        directory.path().c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor < 0) throw std::runtime_error("failed to open test root");
    SourceArtifactInstallTrustedStateStore store =
        SourceArtifactInstallTrustedStateStore::open_below_runtime_parent(
            descriptor, geteuid());
    static_cast<void>(close(descriptor));
    return store;
}

TrustedAlpmReceiptStateStore open_selected_store(
    const TemporaryDirectory& directory) {
    const int descriptor = open(
        directory.path().c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor < 0) throw std::runtime_error("failed to open test root");
    TrustedAlpmReceiptStateStore store =
        TrustedAlpmReceiptStateStore::open_below_runtime_parent(
            descriptor, geteuid());
    static_cast<void>(close(descriptor));
    return store;
}

OwnedDescriptor input_text(const std::string& text) {
#ifdef SYS_memfd_create
    const long raw_descriptor = syscall(
        SYS_memfd_create, "moguet-source-receipt-targets", MFD_CLOEXEC);
    if(raw_descriptor < 0) throw std::runtime_error("memfd_create failed");
    OwnedDescriptor descriptor(static_cast<int>(raw_descriptor));
    write_all(descriptor.get(), text.data(), text.size());
    if(lseek(descriptor.get(), 0, SEEK_SET) == -1) {
        throw std::runtime_error("failed to rewind input text");
    }
    return descriptor;
#else
    static_cast<void>(text);
    throw std::runtime_error("memfd_create is unavailable");
#endif
}

void test_protocol_is_fixed_owner_and_closed() {
    const std::string token = transaction_token('a');
    const std::vector<std::string> valid{
        "prepare", token, "moguet-source-transport-base",
        "AsDependency", "0", "1", "--", "0",
        "moguet-source-transport-test", "1-1",
        "moguet-source-transport-base", "any", "64", "0", std::string(64, 'a'), "-"};
    const auto parsed =
        parse_source_artifact_install_trusted_helper_arguments(valid);
    expect(
        std::holds_alternative<
            SourceArtifactInstallTrustedHelperInvocation>(parsed),
        "valid source helper protocol was rejected");

    std::vector<std::string> owner_injection = valid;
    owner_injection.insert(
        owner_injection.begin() + 2, "selected-repository-provider");
    expect(
        std::holds_alternative<
            SourceArtifactInstallTrustedProtocolFailure>(
            parse_source_artifact_install_trusted_helper_arguments(
                owner_injection)),
        "source helper accepted a caller-selected owner");
    expect(
        std::holds_alternative<
            SourceArtifactInstallTrustedProtocolFailure>(
            parse_source_artifact_install_trusted_helper_arguments(
                {"consume", token, "/tmp/output"})),
        "source helper accepted an output path");
    expect(
        source_artifact_install_trusted_owner() ==
            "source-artifact-install",
        "source helper owner changed");

    std::vector<std::string> two_children = valid;
    two_children.insert(
        two_children.end(),
        {"1", "moguet-source-transport-other", "1-1",
         "moguet-source-transport-base", "any", "64", "0", std::string(64, 'b'), "-"});
    expect(
        std::holds_alternative<SourceArtifactInstallTrustedHelperInvocation>(
            parse_source_artifact_install_trusted_helper_arguments(two_children)),
        "valid two-child v2 positive control was rejected");
    auto duplicate_child = two_children;
    duplicate_child[17] = duplicate_child[8]; // Only the package name differs from the positive control.
    const auto duplicate_name = parse_source_artifact_install_trusted_helper_arguments(duplicate_child);
    expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(duplicate_name) &&
               std::get<SourceArtifactInstallTrustedProtocolFailure>(duplicate_name).issue ==
                   SourceArtifactInstallTrustedProtocolIssueKind::DuplicatePackageName,
           "valid-arity duplicate child was not rejected for duplicate package name");
    duplicate_child = two_children;
    duplicate_child[16] = duplicate_child[7];
    const auto duplicate_index = parse_source_artifact_install_trusted_helper_arguments(duplicate_child);
    expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(duplicate_index) &&
               std::get<SourceArtifactInstallTrustedProtocolFailure>(duplicate_index).issue ==
                   SourceArtifactInstallTrustedProtocolIssueKind::DuplicateArtifactIndex,
           "valid-arity duplicate index was not rejected for duplicate index");
    std::vector<std::string> wrong_package_base = valid;
    wrong_package_base[10] = "other-package-base";
    expect(
        std::holds_alternative<
            SourceArtifactInstallTrustedProtocolFailure>(
            parse_source_artifact_install_trusted_helper_arguments(
                wrong_package_base)),
        "source helper accepted a mismatched artifact PackageBase");
    std::cout << "F-A03: valid v2 positive control / duplicate name / duplicate index PASS\n";
}

void test_state_staging_receipt_replay_and_owner_isolation() {
    ActualArchiveFixture archives;
    const fs::path archive = archives.create_archive(
        "moguet-source-transport-test", "1-1",
        "moguet-source-transport-base", "any");
    TemporaryDirectory runtime("moguet-source-receipt-state");
    SourceArtifactInstallTrustedStateStore source = open_source_store(runtime);
    TrustedAlpmReceiptStateStore selected = open_selected_store(runtime);

    const std::string token = transaction_token('b');
    const auto request = root_request(token, archive);
    OwnedDescriptor input = sealed_input_from_files({archive});
    const auto response = source.prepare(request, input.get());
    expect(
        response.hook_directory ==
                source_artifact_install_hook_directory(token) &&
            response.artifacts.size() == 1 &&
            response.artifacts[0].path ==
                source_artifact_install_staged_artifact_path(token, 0),
        "source state returned an unexpected fixed staging response");

    OwnedDescriptor targets = input_text(
        "moguet-source-transport-test\nsolver-introduced\n");
    expect(source.execute(token) == 0, "fixture execution failed");
    source.record(token, targets.get());
    const auto receipt = parse_source_artifact_install_root_receipt(
        source.consume(token));
    const auto* complete = std::get_if<SourceArtifactInstallRootReceipt>(
        &receipt);
    expect(
        complete != nullptr &&
            complete->state ==
                SourceArtifactInstallRootReceiptState::Complete &&
            complete->installed_package_names ==
                std::vector<std::string>{
                    "moguet-source-transport-test",
                    "solver-introduced"},
        "source state did not retain the factual Install set");
    expect_failure(
        [&]() { static_cast<void>(source.consume(token)); },
        "second source consume succeeded");
    expect_failure(
        [&]() { source.abort(token); },
        "abort after source consume succeeded");

    const std::string selected_token = transaction_token('c');
    selected.prepare(selected_token, {"selected-package"});
    expect_failure(
        [&]() { static_cast<void>(source.consume(selected_token)); },
        "selected-provider token crossed into source state");

    const std::string source_token = transaction_token('d');
    auto source_request = root_request(source_token, archive);
    OwnedDescriptor source_input = sealed_input_from_files({archive});
    static_cast<void>(source.prepare(source_request, source_input.get()));
    expect_failure(
        [&]() { static_cast<void>(selected.consume(source_token)); },
        "source token crossed into selected-provider state");
    source.abort(source_token);
    selected.abort(selected_token);

    const std::string first = transaction_token('e');
    const std::string second = transaction_token('f');
    auto first_request = root_request(first, archive);
    auto second_request = root_request(second, archive);
    OwnedDescriptor first_input = sealed_input_from_files({archive});
    OwnedDescriptor second_input = sealed_input_from_files({archive});
    static_cast<void>(source.prepare(first_request, first_input.get()));
    static_cast<void>(source.prepare(second_request, second_input.get()));
    expect(
        fs::is_directory(
            runtime.path() / "moguet" / "source-artifact-installs" /
            "active" / first) &&
            fs::is_directory(
                runtime.path() / "moguet" /
                "source-artifact-installs" / "active" / second),
        "independent source transactions did not coexist");
    OwnedDescriptor duplicate_active_input = sealed_input_from_files({archive});
    expect_failure(
        [&]() {
            static_cast<void>(source.prepare(
                first_request, duplicate_active_input.get()));
        },
        "active source token was prepared twice");
    source.abort(first);
    source.abort(second);

    const std::string stale = transaction_token('9');
    auto stale_request = root_request(stale, archive);
    OwnedDescriptor stale_input = sealed_input_from_files({archive});
    static_cast<void>(source.prepare(stale_request, stale_input.get()));
    {
        std::ofstream unexpected(
            runtime.path() / "moguet" / "source-artifact-installs" /
            "active" / stale / "unexpected");
        unexpected << "unexpected\n";
    }
    expect_failure(
        [&]() { static_cast<void>(source.consume(stale)); },
        "source consume ignored stale extra state");
    expect_failure(
        [&]() { source.abort(stale); },
        "source abort deleted stale extra state");

    const std::string mismatch = transaction_token('1');
    auto mismatch_request = root_request(mismatch, archive);
    mismatch_request.artifacts[0].full_version = "2-1";
    OwnedDescriptor mismatch_input = sealed_input_from_files({archive});
    expect_failure(
        [&]() {
            static_cast<void>(source.prepare(
                mismatch_request, mismatch_input.get()));
        },
        "root staging accepted mismatched archive metadata");
    expect(
        !fs::exists(
            runtime.path() / "moguet" / "source-artifact-installs" /
            "active" / mismatch),
        "metadata mismatch left active source state");

#ifdef SYS_memfd_create
    const long unsealed_raw = syscall(
        SYS_memfd_create, "moguet-unsealed-source", MFD_CLOEXEC);
    expect(unsealed_raw >= 0, "failed to create unsealed input");
    OwnedDescriptor unsealed(static_cast<int>(unsealed_raw));
    std::ifstream archive_input(archive, std::ios::binary);
    std::vector<char> bytes{
        std::istreambuf_iterator<char>(archive_input),
        std::istreambuf_iterator<char>()};
    write_all(unsealed.get(), bytes.data(), bytes.size());
    const std::string unsealed_token = transaction_token('2');
    auto unsealed_request = root_request(unsealed_token, archive);
    expect_failure(
        [&]() {
            static_cast<void>(source.prepare(
                unsealed_request, unsealed.get()));
        },
        "root staging accepted mutable input bytes");
#endif
}


std::string read_fixture_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(static_cast<bool>(input), "fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_fixture_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    expect(static_cast<bool>(output), "fixture write failed");
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
}

std::string change_only_tar_header(std::string bytes) {
    expect(bytes.size() >= 1024, "raw tar fixture too short");
    // Change the first member's archive-header timestamp and its checksum,
    // preserving every member byte, archive length and package metadata.
    bytes[146] = bytes[146] == '1' ? '2' : '1';
    std::fill(bytes.begin() + 148, bytes.begin() + 156, ' ');
    unsigned checksum = 0;
    for(std::size_t i = 0; i < 512; ++i)
        checksum += static_cast<unsigned char>(bytes[i]);
    char checksum_text[8]{};
    expect(std::snprintf(checksum_text, sizeof(checksum_text), "%06o", checksum) == 6, "tar checksum overflow");
    std::copy(checksum_text, checksum_text + 6, bytes.begin() + 148);
    bytes[154] = '\0';
    bytes[155] = ' ';
    return bytes;
}

class SealedStageFixture {
public:
    ActualArchiveFixture archives;
    fs::path archive;
    TemporaryDirectory runtime;
    SourceArtifactInstallTrustedStateStore store;
    const std::string token = transaction_token('a');
    SourceArtifactInstallRootPrepareRequest request;
    fs::path transaction;
    fs::path stage;
    std::string staged_identity;

    explicit SealedStageFixture(bool signature = false, bool exact = false)
        : archive(archives.create_archive("moguet-source-transport-test", "1-1",
                                          "moguet-source-transport-base", "any", true)),
          runtime("moguet-sealing-regression"), store(open_source_store(runtime)),
          request(root_request(token, archive)) {
        if(signature) write_fixture_bytes(archive.string() + ".sig", "deliberately invalid signature\n");
        request = root_request(token, archive);
        if(exact) {
            request.purpose = SourceArtifactInstallTrustedPurpose::ExactInstalledBinding;
            request.artifacts[0].artifact_index = 4;
            request.artifacts[0].raw_mtree_sha256 = std::string(64, 'd');
        }
        std::vector<fs::path> inputs{archive};
        if(signature) inputs.emplace_back(archive.string() + ".sig");
        auto sealed = sealed_input_from_files(inputs);
        const auto response = store.prepare(request, sealed.get());
        staged_identity = response.staged_identity_sha256;
        transaction = runtime.path() / "moguet/source-artifact-installs/active" / token;
        stage = transaction / "artifacts" / fs::path(response.artifacts[0].path).filename();
        expect(read_fixture_bytes(stage) == read_fixture_bytes(archive), "initial sealed copy was not exact");
    }

    void replace(bool identical, bool signature = false) {
        const fs::path selected = signature ? fs::path(stage.string() + ".sig") : stage;
        const auto original = read_fixture_bytes(selected);
        auto bytes = original;
        if(!identical) {
            if(signature)
                bytes[0] = bytes[0] == 'x' ? 'y' : 'x';
            else
                bytes = change_only_tar_header(std::move(bytes));
        }
        struct stat before{}, after{};
        expect(lstat(selected.c_str(), &before) == 0, "original stat failed");
        const auto replacement = runtime.path() / "replacement";
        write_fixture_bytes(replacement, bytes);
        fs::rename(replacement, selected);
        expect(lstat(selected.c_str(), &after) == 0 && before.st_ino != after.st_ino &&
                   before.st_size == after.st_size,
               "fixture did not replace generation at equal size");
        if(!identical) expect(fixture_sha256(selected) != (signature
                                                               ? request.artifacts[0].signature_sha256
                                                               : request.artifacts[0].archive_sha256),
                              "counterexample did not change exact digest");
    }

    void record() {
        auto targets = input_text("moguet-source-transport-test\n");
        store.record(token, targets.get());
    }
};

template <typename Function>
void expect_sealing_refusal(Function&& action, SourceArtifactInstallSealingFailure expected) {
    try {
        action();
    } catch(const SourceArtifactInstallTrustedStateError& error) {
        expect(error.refusal().reason == expected, "wrong typed sealing refusal");
        return;
    }
    throw std::runtime_error("staged authority failure was accepted");
}

void await_pipe_event(int descriptor) {
    pollfd event{descriptor, POLLIN, 0};
    int result;
    do {
        result = poll(&event, 1, 10000);
    } while(result < 0 && errno == EINTR);
    expect(result == 1 && (event.revents & POLLIN), "ordered process event timed out or closed");
    char byte = 0;
    expect(read(descriptor, &byte, 1) == 1 && byte == 'x', "ordered process event failed");
}

void send_pipe_event(int descriptor) {
    write_all(descriptor, "x", 1);
}

class EventPipe final {
public:
    EventPipe() {
        expect(pipe2(descriptors_, O_CLOEXEC) == 0, "event pipe creation failed");
    }
    EventPipe(const EventPipe&) = delete;
    EventPipe& operator=(const EventPipe&) = delete;
    ~EventPipe() {
        static_cast<void>(close(descriptors_[0]));
        static_cast<void>(close(descriptors_[1]));
    }
    int reader() const {
        return descriptors_[0];
    }
    int writer() const {
        return descriptors_[1];
    }

private:
    int descriptors_[2]{-1, -1};
};

// The child always exits without unwinding the parent's temporary directories.
// On a failed assertion the owner reaps it, including while paused in a seam.
class OrderedProcess final {
public:
    explicit OrderedProcess(const std::function<void(int, int)>& action) {
        child_ = fork();
        expect(child_ >= 0, "ordered process fork failed");
        if(child_ == 0) {
            try {
                action(ready_.writer(), resume_.reader());
                _exit(0);
            } catch(const std::exception& error) {
                std::cerr << "ordered child: " << error.what() << '\n';
                _exit(1);
            }
        }
    }
    OrderedProcess(const OrderedProcess&) = delete;
    OrderedProcess& operator=(const OrderedProcess&) = delete;
    ~OrderedProcess() {
        if(child_ <= 0) return;
        static_cast<void>(kill(child_, SIGKILL));
        while(waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    void await_ready() {
        await_pipe_event(ready_.reader());
    }
    void resume() {
        send_pipe_event(resume_.writer());
    }
    void finish() {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child_, &status, 0);
        } while(waited < 0 && errno == EINTR);
        expect(waited == child_, "ordered child was not reaped");
        child_ = -1;
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "ordered child failed");
    }

private:
    EventPipe ready_;
    EventPipe resume_;
    pid_t child_ = -1;
};

template <typename Function>
void expect_lifetime_busy(Function&& action) {
    try {
        action();
    } catch(const SourceArtifactInstallTrustedStateError& error) {
        expect(error.refusal().reason == SourceArtifactInstallSealingFailure::TransactionLifetimeBusy &&
                   (error.refusal().error_number == EWOULDBLOCK || error.refusal().error_number == EAGAIN),
               "lifetime contention was not a typed busy refusal");
        return;
    }
    throw std::runtime_error("concurrent operation crossed the transaction lifetime lease");
}

void test_cleanup_first_ordering(bool consume) {
    using Event = SourceArtifactInstallTrustedStateTestEvent;
    SealedStageFixture fixture(true);
    OrderedProcess cleanup([&](int ready, int resume) {
        bool paused = false;
        set_source_artifact_install_trusted_state_test_hook([&](Event event, int, const auto&) {
            // This existing seam is reached by the audited cleanup too: the
            // regression fails there because cleanup has not taken any lock.
            if(event == Event::AfterArtifactMetadataValidation && !paused) {
                paused = true;
                send_pipe_event(ready);
                await_pipe_event(resume);
            }
        });
        if(consume) {
            const auto receipt = parse_source_artifact_install_root_receipt(fixture.store.consume(fixture.token));
            expect(std::get<SourceArtifactInstallRootReceipt>(receipt).state == SourceArtifactInstallRootReceiptState::Missing,
                   "unexecuted cleanup manufactured Complete");
        } else
            fixture.store.abort(fixture.token);
        expect(paused && !fs::exists(fixture.stage), "cleanup did not complete after exclusion");
    });
    cleanup.await_ready();
    std::size_t launches = 0;
    set_source_artifact_install_trusted_exec_test_hook([&](const auto&) { ++launches; return 0; });
    expect_lifetime_busy([&] { static_cast<void>(fixture.store.execute(fixture.token)); });
    expect(launches == 0 && !fs::exists(fixture.transaction / "execution") &&
               !fs::exists(fixture.transaction / "authorized") && fs::exists(fixture.stage),
           "cleanup-first execute reached a claim, authorization or package-manager launch");
    cleanup.resume();
    cleanup.finish();
    expect(!fs::exists(fixture.transaction), "cleanup-first left an active transaction");
    set_source_artifact_install_trusted_exec_test_hook([](const auto&) { return 0; });
    std::cout << "F-A01: cleanup-first " << (consume ? "consume" : "abort") << " excludes execute PASS\n";
}

void test_execute_first_ordering(bool consume, bool final_handoff) {
    using Event = SourceArtifactInstallTrustedStateTestEvent;
    SealedStageFixture fixture(true);
    OrderedProcess executor([&](int ready, int resume) {
        bool paused = false;
        const auto pause = [&] {
            paused = true;
            send_pipe_event(ready);
            await_pipe_event(resume);
            expect(fs::exists(fixture.stage) && fs::exists(fixture.stage.string() + ".sig"),
                   "execution input disappeared while executor held the lease");
        };
        set_source_artifact_install_trusted_state_test_hook([&](Event event, int, const auto&) {
            if(!final_handoff && event == Event::BeforeFinalReproof) pause();
        });
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) {
            if(final_handoff) pause();
            return 0;
        });
        expect(fixture.store.execute(fixture.token) == 0 && paused, "execute-first seam was not reached");
    });
    executor.await_ready();
    expect_lifetime_busy([&] {
        if(consume)
            static_cast<void>(fixture.store.consume(fixture.token));
        else
            fixture.store.abort(fixture.token);
    });
    std::size_t second_launches = 0;
    set_source_artifact_install_trusted_exec_test_hook([&](const auto&) { ++second_launches; return 0; });
    expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                           SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch);
    expect(second_launches == 0 && fs::exists(fixture.stage), "double execute or live stage deletion succeeded");
    if(final_handoff) {
        // Separate helper open/SH lock, as in the real PostTransaction hook.
        auto recorder = open_source_store(fixture.runtime);
        auto targets = input_text("moguet-source-transport-test\n");
        recorder.record(fixture.token, targets.get());
        expect(fs::exists(fixture.transaction / "receipt") && fs::exists(fixture.stage),
               "record blocked or retired a live transaction");
        expect_lifetime_busy([&] { static_cast<void>(fixture.store.consume(fixture.token)); });
    }
    executor.resume();
    executor.finish();
    expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                           SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch);
    expect(second_launches == 0, "released lifetime lease allowed execution replay");
    const auto receipt = parse_source_artifact_install_root_receipt(fixture.store.consume(fixture.token));
    expect(std::get<SourceArtifactInstallRootReceipt>(receipt).state ==
               (final_handoff ? SourceArtifactInstallRootReceiptState::Complete : SourceArtifactInstallRootReceiptState::Missing),
           "post-execution cleanup did not follow the recorded lifecycle");
    set_source_artifact_install_trusted_exec_test_hook([](const auto&) { return 0; });
    std::cout << "F-A01: execute-first " << (consume ? "consume" : "abort")
              << (final_handoff ? " after authorization / record" : " before final reproof")
              << " busy; double execute rejected; post-exit consume PASS\n";
}

// A real exec target for the helper's existing test seam. No pacman transaction
// occurs: only inherited FDs and private fixture pathname opens are exercised.
int lease_exec_child(int argc, char* argv[]) {
    expect(argc == 8, "invalid lease exec fixture arguments");
    const int ready = std::stoi(argv[2]), resume = std::stoi(argv[3]), sentinel = std::stoi(argv[4]);
    const fs::path transaction = argv[5], stage = argv[6];
    expect(fcntl(sentinel, F_GETFD) == -1 && errno == EBADF, "unrelated CLOEXEC descriptor leaked");
    int lifetime = -1;
    for(const auto& entry : fs::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        const auto target = fs::read_symlink(entry.path(), error);
        if(error || !target.string().starts_with(transaction.parent_path().parent_path().parent_path().string())) continue;
        expect(target == transaction / "lifetime" && lifetime == -1,
               "helper namespace/stage descriptor leaked across exec");
        lifetime = std::stoi(entry.path().filename().string());
    }
    expect(lifetime >= 0 && (fcntl(lifetime, F_GETFD) & FD_CLOEXEC) == 0, "lifetime FD did not survive exec");
    if(std::string(argv[7]) == "descendant") {
        EventPipe inherited;
        const pid_t child = fork();
        expect(child >= 0, "lease descendant fork failed");
        if(child != 0) {
            expect(close(lifetime) == 0, "executor could not release its lease copy");
            send_pipe_event(inherited.writer());
            int status = 0;
            expect(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "lease descendant failed");
            return 0;
        }
        await_pipe_event(inherited.reader()); // Only the descendant owns the lease now.
    }
    send_pipe_event(ready);
    await_pipe_event(resume);
    OwnedDescriptor package(open(stage.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    OwnedDescriptor signature(open((stage.string() + ".sig").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    expect(package.get() >= 0 && signature.get() >= 0,
           "package-manager pathname open lost its inputs after final handoff");
    return 0;
}

void test_lease_survives_exec(bool descendant) {
    SealedStageFixture fixture(true);
    OrderedProcess executor([&](int ready, int resume) {
        OwnedDescriptor sentinel(open("/dev/null", O_RDONLY | O_CLOEXEC));
        expect(sentinel.get() >= 0, "sentinel open failed");
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) -> int {
            for(int descriptor : {ready, resume})
                expect(fcntl(descriptor, F_SETFD, 0) == 0, "fixture synchronization FD inheritance failed");
            const auto ready_text = std::to_string(ready), resume_text = std::to_string(resume), sentinel_text = std::to_string(sentinel.get());
            execl("/proc/self/exe", "source-artifact-install-trusted-transport-test", "--lease-exec-child",
                  ready_text.c_str(), resume_text.c_str(), sentinel_text.c_str(), fixture.transaction.c_str(), fixture.stage.c_str(),
                  descendant ? "descendant" : "direct", static_cast<char*>(nullptr));
            throw std::runtime_error("lease fixture exec failed");
        });
        static_cast<void>(fixture.store.execute(fixture.token));
        throw std::runtime_error("lease fixture did not exec");
    });
    executor.await_ready();
    OwnedDescriptor contender(open((fixture.transaction / "lifetime").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    expect(contender.get() >= 0 && flock(contender.get(), LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK,
           "kernel flock did not survive helper exec/descendant handoff");
    expect_lifetime_busy([&] { fixture.store.abort(fixture.token); });
    expect_lifetime_busy([&] { static_cast<void>(fixture.store.consume(fixture.token)); });
    fixture.record();
    expect(fs::exists(fixture.stage), "live exec input was removed");
    executor.resume();
    executor.finish();
    expect(flock(contender.get(), LOCK_EX | LOCK_NB) == 0, "lease remained locked after process end");
    expect(flock(contender.get(), LOCK_UN) == 0, "contender unlock failed");
    const auto receipt = parse_source_artifact_install_root_receipt(fixture.store.consume(fixture.token));
    expect(std::get<SourceArtifactInstallRootReceipt>(receipt).state == SourceArtifactInstallRootReceiptState::Complete,
           "post-exec consume was permanently blocked");
    std::cout << "F-A01: fork/exec/flock " << (descendant ? "descendant" : "direct")
              << " inheritance, descriptor isolation, delayed pathname open, release/cleanup PASS\n";
}

void test_record_lifetime_ordering() {
    SealedStageFixture fixture;
    expect(fixture.store.execute(fixture.token) == 0, "record lifetime fixture execution failed");
    OrderedProcess recorder([&](int ready, int resume) {
        set_source_artifact_install_trusted_state_test_hook([&](auto event, int, const auto&) {
            if(event == SourceArtifactInstallTrustedStateTestEvent::AfterArtifactMetadataValidation) {
                send_pipe_event(ready);
                await_pipe_event(resume);
            }
        });
        fixture.record();
    });
    recorder.await_ready();
    expect_lifetime_busy([&] { fixture.store.abort(fixture.token); });
    expect_lifetime_busy([&] { static_cast<void>(fixture.store.consume(fixture.token)); });
    expect(fs::exists(fixture.stage), "record lost its stage during receipt publication");
    recorder.resume();
    recorder.finish();
    const auto receipt = parse_source_artifact_install_root_receipt(fixture.store.consume(fixture.token));
    expect(std::get<SourceArtifactInstallRootReceipt>(receipt).state == SourceArtifactInstallRootReceiptState::Complete,
           "record lease prevented subsequent consume");
    std::cout << "F-A01: record-first excludes abort/consume through receipt publication PASS\n";
}

void test_lifetime_lease_identity() {
    using Reason = SourceArtifactInstallSealingFailure;
    for(const std::string kind : {"missing", "symlink", "directory", "hardlink", "mode", "token", "other-transaction", "generation", "group"}) {
        SealedStageFixture fixture;
        const auto lease = fixture.transaction / "lifetime";
        const auto bytes = read_fixture_bytes(lease);
        OwnedDescriptor old(open(lease.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        expect(old.get() >= 0, "prepare did not publish its mandatory lease");
        Reason reason = Reason::StagedArtifactRevalidationFailure;
        if(kind == "missing") fs::remove(lease);
        if(kind == "symlink") {
            fs::remove(lease);
            fs::create_symlink(fixture.archive, lease);
        }
        if(kind == "directory") {
            fs::remove(lease);
            fs::create_directory(lease);
        }
        if(kind == "hardlink") fs::create_hard_link(lease, fixture.runtime.path() / "alias");
        if(kind == "mode") fs::permissions(lease, fs::perms::group_write, fs::perm_options::add);
        if(kind == "token") {
            auto stale = bytes;
            stale.replace(stale.find(fixture.token), fixture.token.size(), transaction_token('b'));
            write_fixture_bytes(lease, stale);
            reason = Reason::TrustedTransportProtocolMismatch;
        }
        if(kind == "generation") {
            const auto replacement = fixture.runtime.path() / "replacement-lease";
            write_fixture_bytes(replacement, bytes);
            fs::rename(replacement, lease);
            reason = Reason::StagedArtifactGenerationMismatch;
        }
        if(kind == "other-transaction") {
            const auto other_token = transaction_token('b');
            const auto other_request = root_request(other_token, fixture.archive);
            auto input = sealed_input_from_files({fixture.archive});
            static_cast<void>(fixture.store.prepare(other_request, input.get()));
            fs::rename(fixture.transaction.parent_path() / other_token / "lifetime", lease);
            reason = Reason::TrustedTransportProtocolMismatch;
        }
        if(kind == "group") {
            // Change only the saved group authority, without requiring chown
            // privileges. The actual lease group must still match its prepare.
            auto identity = read_fixture_bytes(fixture.transaction / "identity");
            auto group = identity.find("lifetime\t");
            expect(group != std::string::npos, "lease missing from generation authority");
            for(int field = 0; field < 5; ++field)
                group = identity.find('\t', group) + 1;
            const auto end = identity.find('\t', group);
            identity.replace(group, end - group, "4294967294");
            write_fixture_bytes(fixture.transaction / "identity", identity);
            reason = Reason::StagedArtifactGenerationMismatch;
        }
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); }, reason);
        expect_sealing_refusal([&] { fixture.store.abort(fixture.token); }, reason);
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.consume(fixture.token)); }, reason);
        expect_sealing_refusal([&] { fixture.record(); }, reason);
        expect(fs::exists(fixture.stage) && !fs::exists(fixture.transaction / "authorized"),
               "invalid lease authorized execution or removed evidence");
    }
    std::cout << "F-A01: mandatory lease type/mode/token/group/generation failures closed PASS\n";
}

void test_lifetime_races() {
    for(bool consume : {false, true}) {
        test_cleanup_first_ordering(consume);
        for(bool final_handoff : {false, true})
            test_execute_first_ordering(consume, final_handoff);
    }
    for(bool descendant : {false, true})
        test_lease_survives_exec(descendant);
    test_record_lifetime_ordering();
    test_lifetime_lease_identity();
}

void test_sealing_state_regressions() {
    using Reason = SourceArtifactInstallSealingFailure;
    using Event = SourceArtifactInstallTrustedStateTestEvent;
    std::size_t launches = 0;
    set_source_artifact_install_trusted_exec_test_hook([&](const auto& arguments) {
        ++launches;
        expect(arguments[0] == "/usr/bin/pacman" && arguments[1] == "-U" &&
                   arguments.back().starts_with("/run/moguet/source-artifact-installs/active/") &&
                   arguments.back().find("/proc/") == std::string::npos,
               "helper changed pacman pathname/signature semantics");
        return 0;
    });

    for(bool identical : {false, true}) {
        SealedStageFixture fixture;
        fixture.replace(identical);
        const auto expected = identical ? Reason::StagedArtifactGenerationMismatch
                                        : Reason::StagedArtifactDigestMismatch;
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); }, expected);
        const auto status = fixture.store.execution_status(fixture.token);
        expect(!status.authorized && status.refusal && status.refusal->reason == expected,
               "failed final authority became launch authorization");
        expect_sealing_refusal([&] { fixture.record(); }, expected);
        expect(!fs::exists(fixture.transaction / "receipt"), "sealing failure published Complete receipt");
    }
    expect(launches == 0, "replacement launched package manager");

    for(Event point : {Event::AfterArtifactMetadataValidation, Event::BeforeFinalReproof}) {
        SealedStageFixture fixture;
        bool injected = false;
        set_source_artifact_install_trusted_state_test_hook([&](Event event, int, const auto&) {
            if(event == point && !injected) {
                injected = true;
                fixture.replace(false);
            }
        });
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               point == Event::AfterArtifactMetadataValidation ? Reason::StagedArtifactReplacement
                                                                               : Reason::StagedArtifactDigestMismatch);
        set_source_artifact_install_trusted_state_test_hook({});
        expect(injected && !fixture.store.execution_status(fixture.token).authorized,
               "reproof race was not exercised or authorized");
    }
    expect(launches == 0, "reproof race launched package manager");

    for(bool before_consume : {false, true}) {
        SealedStageFixture fixture;
        expect(fixture.store.execute(fixture.token) == 0, "unchanged stage did not authorize");
        if(before_consume) fixture.record();
        fixture.replace(false);
        expect_sealing_refusal([&] {
            if(before_consume)
                static_cast<void>(fixture.store.consume(fixture.token));
            else
                fixture.record();
        },
                               Reason::StagedArtifactDigestMismatch);
        if(!before_consume) expect(!fs::exists(fixture.transaction / "receipt"), "record emitted Complete after replacement");
    }

    {
        SealedStageFixture fixture;
        // An in-place content mutation keeps the inode but must fail the digest.
        write_fixture_bytes(fixture.stage, change_only_tar_header(read_fixture_bytes(fixture.stage)));
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::StagedArtifactDigestMismatch);
    }
    {
        SealedStageFixture fixture;
        fs::resize_file(fixture.stage, fs::file_size(fixture.stage) + 1);
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::StagedArtifactRevalidationFailure);
    }
    {
        SealedStageFixture fixture;
        fs::remove(fixture.stage);
        fs::create_symlink(fixture.archive, fixture.stage);
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::StagedArtifactRevalidationFailure);
    }
    {
        SealedStageFixture fixture;
        fs::create_hard_link(fixture.stage, fixture.runtime.path() / "foreign-alias");
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::StagedArtifactRevalidationFailure);
    }
    {
        SealedStageFixture fixture;
        fs::permissions(fixture.stage, fs::perms::group_write, fs::perm_options::add);
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::StagedArtifactRevalidationFailure);
    }
    {
        SealedStageFixture fixture;
        const auto active = fixture.transaction.parent_path();
        const auto saved = fixture.runtime.path() / "saved-active";
        fs::rename(active, saved);
        fs::copy(saved, active, fs::copy_options::recursive);
        auto fresh_helper = open_source_store(fixture.runtime);
        expect_sealing_refusal([&] { static_cast<void>(fresh_helper.execute(fixture.token)); },
                               Reason::StagedArtifactGenerationMismatch);
    }
    {
        SealedStageFixture fixture;
        write_fixture_bytes(fixture.transaction / "stale-state", "unexpected state\n");
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::TrustedTransportProtocolMismatch);
        expect(!fixture.store.execution_status(fixture.token).authorized,
               "unexpected transaction state authorized execution");
    }
    for(bool missing : {false, true}) {
        SealedStageFixture fixture;
        if(missing)
            fs::remove(fixture.transaction / "identity");
        else {
            auto bytes = read_fixture_bytes(fixture.transaction / "prepared");
            const auto version = bytes.find("PREPARED\t2");
            expect(version != std::string::npos, "fixture missing schema");
            bytes[version + std::string("PREPARED\t").size()] = '1';
            write_fixture_bytes(fixture.transaction / "prepared", bytes);
        }
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               Reason::TrustedTransportProtocolMismatch);
    }

    for(bool identical : {false, true}) {
        SealedStageFixture fixture(true);
        fixture.replace(identical, true);
        expect_sealing_refusal([&] { static_cast<void>(fixture.store.execute(fixture.token)); },
                               identical ? Reason::StagedArtifactGenerationMismatch : Reason::SignatureDigestMismatch);
    }
    {
        SealedStageFixture fixture(true);
        TemporaryDirectory gpg("moguet-sealing-gpg");
        TemporaryDirectory db("moguet-sealing-db");
        alpm_errno_t error = ALPM_ERR_OK;
        alpm_handle_t* handle = alpm_initialize("/", db.path().c_str(), &error);
        expect(handle != nullptr, "signature fixture handle failed");
        expect(alpm_option_set_gpgdir(handle, gpg.path().c_str()) == 0, "signature fixture gpgdir failed");
        alpm_pkg_t* package = nullptr;
        expect(alpm_pkg_load(handle, fixture.stage.c_str(), 0,
                             ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL, &package) != 0 &&
                   alpm_errno(handle) == ALPM_ERR_PKG_INVALID_SIG,
               "staged invalid adjacent signature was silently omitted");
        // Signature acceptance remains entirely the configured ALPM policy.
        expect(alpm_pkg_load(handle, fixture.stage.c_str(), 0, 0, &package) == 0,
               "accepted-signature policy changed");
        alpm_pkg_free(package);
        alpm_release(handle);
        expect(read_fixture_bytes(fixture.stage.string() + ".sig") ==
                   read_fixture_bytes(fixture.archive.string() + ".sig"),
               "signature bytes changed during transport");
        expect(fixture.store.execute(fixture.token) == 0, "signature staging altered launch semantics");
        fixture.record();
        auto receipt = parse_source_artifact_install_root_receipt(fixture.store.consume(fixture.token));
        expect(std::get<SourceArtifactInstallRootReceipt>(receipt).state == SourceArtifactInstallRootReceiptState::Complete,
               "unchanged Install receipt regressed");
    }
    {
        SealedStageFixture fixture;
        TemporaryDirectory db("moguet-sealing-optional-db");
        alpm_errno_t error = ALPM_ERR_OK;
        alpm_handle_t* handle = alpm_initialize("/", db.path().c_str(), &error);
        expect(handle != nullptr, "optional-signature fixture handle failed");
        alpm_pkg_t* package = nullptr;
        expect(alpm_pkg_load(handle, fixture.stage.c_str(), 0,
                             ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL, &package) == 0,
               "missing optional signature semantics changed");
        alpm_pkg_free(package);
        alpm_release(handle);
    }
    {
        SealedStageFixture fixture;
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) {
            expect_failure([&] { fixture.store.abort(fixture.token); }, "in-flight abort removed input");
            expect_failure([&] { static_cast<void>(fixture.store.consume(fixture.token)); }, "in-flight consume removed input");
            expect(fs::exists(fixture.stage), "in-flight stage disappeared");
            fixture.record(); // PostTransaction hook is allowed while pacman holds the lease.
            expect_failure([&] { fixture.store.abort(fixture.token); }, "record released the execution lease");
            return 0;
        });
        expect(fixture.store.execute(fixture.token) == 0, "execution lease fixture failed");
        expect_failure([&] { static_cast<void>(fixture.store.execute(fixture.token)); }, "execution was replayed");
        const auto parsed = parse_source_artifact_install_root_receipt(fixture.store.consume(fixture.token));
        expect(std::get<SourceArtifactInstallRootReceipt>(parsed).state == SourceArtifactInstallRootReceiptState::Complete,
               "execution lease blocked ordinary post-exit consume");
    }
    set_source_artifact_install_trusted_exec_test_hook([](const auto&) { return 0; });
    std::cout << "F-S5-01: digest/generation/reproof/receipt/signature/lease regressions passed\n";
}

void test_sealing_protocol_regressions() {
    ActualArchiveFixture archives;
    const auto archive = archives.create_archive("moguet-source-transport-test", "1-1",
                                                 "moguet-source-transport-base", "any");
    const auto request = root_request(transaction_token('b'), archive);
    const auto protocol = serialize_source_artifact_install_root_prepared_state(request);
    auto malformed = protocol;
    const auto digest = malformed.find(request.artifacts[0].archive_sha256);
    expect(digest != std::string::npos, "digest missing in prepared protocol");
    malformed[digest] = 'G';
    const auto invalid = parse_source_artifact_install_root_prepared_state(malformed);
    expect(std::get<SourceArtifactInstallTrustedProtocolFailure>(invalid).issue ==
               SourceArtifactInstallTrustedProtocolIssueKind::InvalidDigest,
           "malformed digest was accepted");
    malformed = protocol;
    malformed.erase(digest, 65); // Includes one delimiter: legacy arity is refused.
    expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(
               parse_source_artifact_install_root_prepared_state(malformed)),
           "missing digest became legacy success");
    auto old = protocol;
    old[old.find("PREPARED\t2") + std::string("PREPARED\t").size()] = '1';
    expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(
               parse_source_artifact_install_root_prepared_state(old)),
           "old state schema was accepted");
    const auto refusal = SourceArtifactInstallExecutionObservation{request.transaction_token, false,
                                                                   SourceArtifactInstallSealingRefusal{SourceArtifactInstallSealingFailure::StagedArtifactDigestMismatch, EIO}};
    auto roundtrip = parse_source_artifact_install_execution_observation(
        serialize_source_artifact_install_execution_observation(refusal));
    expect(std::get<SourceArtifactInstallExecutionObservation>(roundtrip).refusal == refusal.refusal,
           "typed refusal/system cause did not roundtrip");
}

class TemporaryCacheHome final {
public:
    TemporaryCacheHome() : directory_("moguet-source-transport-cache") {
        const char* previous = std::getenv("XDG_CACHE_HOME");
        if(previous != nullptr) previous_ = previous;
        if(setenv("XDG_CACHE_HOME", directory_.path().c_str(), 1) != 0) {
            throw std::runtime_error("failed to set XDG_CACHE_HOME");
        }
    }

    ~TemporaryCacheHome() noexcept {
        if(previous_.has_value()) {
            static_cast<void>(setenv(
                "XDG_CACHE_HOME", previous_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("XDG_CACHE_HOME"));
        }
    }

private:
    TemporaryDirectory directory_;
    std::optional<std::string> previous_;
};

SourceAwarePackageIdentity expected_identity(
    const ArtifactPackageIdentity& actual,
    const std::string& package_base) {
    const std::string* architecture = actual.architecture.value();
    expect(architecture != nullptr, "actual architecture is unavailable");
    return SourceAwarePackageIdentity::make(
        PackageChildIdentity::make(
            PackageBaseIdentity::make(
                PackageSourceIdentity::aur(
                    SourceLocationIdentity::known_git_remote(
                        "https://aur.archlinux.org/" + package_base +
                        ".git")),
                package_base),
            actual.package_name),
        SourceRevisionIdentity::unknown(),
        PackageVersionIdentity::composite(actual.full_version),
        PackageArchitectureIdentity::known({*architecture}));
}

class PreparedInstallFixture final {
public:
    PreparedInstallFixture(
        const fs::path& archive,
        bool needed = false, bool with_signature = false) {
        workspace_ = std::make_unique<ArtifactWorkspace>(
            create_artifact_workspace(prepare_private_trusted_cache_root(
                prepare_test_trusted_cache_root())));
        const fs::path target = workspace_->path() / archive.filename();
        const std::string output = target.string() + "\n";
        ExpectedPackageArtifactSet expected =
            validate_makepkg_packagelist_output_set(*workspace_, output);
        fs::copy_file(archive, target);
        if(with_signature) write_fixture_bytes(target.string() + ".sig", "retained diagnostic signature\n");
        ValidatedPackageArtifactSet artifacts =
            validate_post_build_package_artifacts(
                std::move(*workspace_), expected);
        workspace_.reset();

        PackageBaseArtifactInstallPreparationResult prepared =
            prepare_package_base_artifact_install(
                artifacts,
                "moguet-source-transport-base",
                {{"moguet-source-transport-base",
                  "moguet-source-transport-test",
                  DesiredInstallReason::Dependency}},
                ArtifactInstallPreparationOptions{needed, false},
                PacmanDatabasePaths{"/", "/var/lib/pacman"});
        if(!prepared.is_prepared() || prepared.prepared() == nullptr) {
            throw std::runtime_error("failed to prepare transport fixture");
        }
        preparation_ = std::make_unique<
            PackageBaseArtifactInstallPreparationResult>(
            std::move(prepared));

        PreparedPackageBaseArtifactInstall* install =
            preparation_->prepared();
        const auto& selected = install->selected_artifacts();
        expect(selected.size() == 1, "fixture selected set is not singular");
        const RootTargetIdentity root{0, "fixture-root"};
        binding_ = std::make_unique<SourceArtifactInstallTrustedBinding>(
            SourceArtifactInstallTrustedBinding{
                {std::nullopt,
                 0,
                 "moguet-source-transport-base",
                 {root}},
                {{selected[0].artifact_index,
                  expected_identity(
                      selected[0].identity,
                      "moguet-source-transport-base"),
                  DesiredInstallReason::Dependency,
                  {PackageRole::BuildDependency},
                  {root}}}});
    }

    PreparedPackageBaseArtifactInstall& install() {
        return *preparation_->prepared();
    }

    const SourceArtifactInstallTrustedBinding& binding() const {
        return *binding_;
    }

private:
    std::unique_ptr<ArtifactWorkspace> workspace_;
    std::unique_ptr<PackageBaseArtifactInstallPreparationResult>
        preparation_;
    std::unique_ptr<SourceArtifactInstallTrustedBinding> binding_;
};

enum class ProcessScenario {
    Complete,
    Missing,
    PacmanFailure,
    MalformedReceipt,
    SealingFailure,
    ConsumeSealingFailure,
    UnknownOutcome,
};

enum class ExecutionStatusScenario {
    Valid,
    QueryFailure,
    CaptureFailure,
    Malformed,
    Truncated,
    CaptureLimit,
    OldSchema,
    FutureSchema,
    Unauthorized,
    Missing,
    WrongToken,
};

enum class HelperExecutionScenario {
    InProcess,
    RefuseBeforeAuthorization,
    RefuseAfterAuthorization,
    SignalBeforeObservation,
    ExecUnobserved,
    ExecPreTransaction,
    ExecPostTransaction,
    SignalAfterObservation,
};

struct ProcessStubState {
    ProcessScenario scenario = ProcessScenario::Complete;
    std::optional<SourceArtifactInstallRootPrepareRequest> request;
    std::size_t capture_count = 0;
    std::size_t run_count = 0;
    std::size_t abort_count = 0;
    ExecutionStatusScenario execution_status = ExecutionStatusScenario::Valid;
    SourceArtifactInstallTrustedStateStore* real_store = nullptr;
    std::size_t prepare_count = 0;
    std::size_t consume_count = 0;
    fs::path transaction;
    std::map<std::string, std::string> private_evidence;
    HelperExecutionScenario helper_execution = HelperExecutionScenario::InProcess;
    bool fail_refusal_publication = false;
    bool refusal_publication_attempted = false;
    int package_exit = 42;
    int helper_exit = 1;
    std::function<void()> before_status;
};

ProcessStubState process_state;

std::map<std::string, std::string> snapshot_private_evidence(const fs::path& transaction) {
    std::map<std::string, std::string> result;
    if(!fs::exists(transaction)) return result;
    const auto inspect = [&](const fs::path& path) {
        struct stat metadata{};
        expect(lstat(path.c_str(), &metadata) == 0, "private evidence stat failed");
        std::string identity = std::to_string(metadata.st_dev) + ":" + std::to_string(metadata.st_ino) +
                               ":" + std::to_string(metadata.st_mode) + ":" + std::to_string(metadata.st_uid) +
                               ":" + std::to_string(metadata.st_gid) + ":" + std::to_string(metadata.st_nlink) +
                               ":" + std::to_string(metadata.st_size) + ":" + std::to_string(metadata.st_mtim.tv_sec) +
                               ":" + std::to_string(metadata.st_mtim.tv_nsec) + ":" + std::to_string(metadata.st_ctim.tv_sec) +
                               ":" + std::to_string(metadata.st_ctim.tv_nsec);
        if(S_ISREG(metadata.st_mode)) identity += ":" + read_fixture_bytes(path);
        result.emplace(path.lexically_relative(transaction).string(), std::move(identity));
    };
    inspect(transaction);
    for(const auto& entry : fs::recursive_directory_iterator(transaction))
        inspect(entry.path());
    return result;
}

std::vector<std::string> helper_arguments(
    const ExplicitProcessInvocation& invocation) {
    expect(
        invocation.executable == "/usr/bin/sudo" &&
            invocation.arguments.size() >= 3 &&
            invocation.arguments[0] == "--" &&
            invocation.arguments[1] ==
                MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH &&
            invocation.environment ==
                std::vector<std::string>{"PATH=/usr/bin", "LC_ALL=C"},
        "source transport helper provenance changed");
    return std::vector<std::string>(
        invocation.arguments.begin() + 2, invocation.arguments.end());
}

void verify_pacman_invocation(const ExplicitProcessInvocation& invocation) {
    expect(process_state.request.has_value(), "execution ran before preparation");
    const auto arguments = helper_arguments(invocation);
    expect(arguments == std::vector<std::string>{"execute", process_state.request->transaction_token} &&
               !invocation.standard_input_fd,
           "outer transport bypassed the privileged reproof/exec owner");
}

int package_manager_phase_child(int argc, char* argv[]) {
    expect(argc == 6, "invalid package-manager phase fixture arguments");
    const fs::path runtime = argv[2];
    const std::string token = argv[3], phase = argv[4];
    const int package_status = std::stoi(argv[5]);
    OwnedDescriptor parent(open(runtime.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    expect(parent.get() >= 0, "phase fixture runtime unavailable");
    auto store = SourceArtifactInstallTrustedStateStore::open_below_runtime_parent(parent.get(), geteuid());
    const auto hooks = runtime / "moguet/source-artifact-installs/active" / token / "hooks";
    if(phase != "none" && phase != "post") {
        const auto contents = read_fixture_bytes(hooks / ("moguet-source-artifact-execution-" + token + ".hook"));
        expect(contents.find("When = PreTransaction\n") != std::string::npos &&
                   contents.find("Exec = " MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH " observe-execution " + token + "\n") != std::string::npos &&
                   contents.find("AbortOnFail") == std::string::npos,
               "generated execution hook changed its fixed owner/phase/policy");
        const auto parsed = parse_source_artifact_install_trusted_helper_arguments({"observe-execution", token});
        const auto& invocation = std::get<SourceArtifactInstallTrustedHelperInvocation>(parsed);
        expect(invocation.command == SourceArtifactInstallTrustedHelperCommand::ObserveExecution, "hook dispatch changed");
        // The actual production producer validates and publishes the marker.
        // This function only runs after a real exec of this fixed test binary;
        // the test never writes a positive marker directly.
        store.observe_execution(invocation.transaction_token);
        expect(store.execution_status(token).execution_evidence == SourceArtifactInstallExecutionEvidence::PreTransaction,
               "PreTransaction producer did not establish positive evidence");
    }
    if(phase == "signal") {
        raise(SIGTERM);
        return 96;
    }
    if(phase == "post" || (phase != "none" && package_status == 0)) {
        const auto contents = read_fixture_bytes(hooks / source_artifact_install_hook_filename(token));
        expect(contents.find("When = PostTransaction\n") != std::string::npos &&
                   contents.find("Operation = Upgrade") == std::string::npos &&
                   contents.find("Exec = " MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH " record " + token + "\n") != std::string::npos,
               "Install-only PostTransaction owner changed");
        auto targets = input_text("moguet-source-transport-test\n");
        store.record(token, targets.get());
    }
    return package_status;
}

int run_helper_in_child() {
    const auto scenario = process_state.helper_execution;
    const auto token = process_state.request->transaction_token;
    const auto runtime = process_state.transaction.parent_path().parent_path().parent_path().parent_path();
    EventPipe refusal_attempted;
    const pid_t child = fork();
    expect(child >= 0, "helper fixture fork failed");
    if(child == 0) {
        const auto fail_exec = [&]() -> int {
            const auto missing = runtime / "absent-package-manager";
            char name[] = "absent-package-manager";
            char* arguments[]{name, nullptr};
            execv(missing.c_str(), arguments);
            const int error_number = errno;
            expect(error_number == ENOENT, "fixture exec attempt did not fail ENOENT");
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::ExecutableLaunchFailure, "fixture execv failed", error_number);
        };
        set_source_artifact_install_trusted_state_test_hook([&](auto event, int, const auto&) {
            if(event == SourceArtifactInstallTrustedStateTestEvent::BeforeFinalReproof &&
               scenario == HelperExecutionScenario::RefuseBeforeAuthorization) static_cast<void>(fail_exec());
            if(event == SourceArtifactInstallTrustedStateTestEvent::BeforeRefusalPublication) {
                expect(fs::exists(process_state.transaction / "authorized") ==
                           (scenario == HelperExecutionScenario::RefuseAfterAuthorization),
                       "refusal failure injection was not at its required authorization phase");
                send_pipe_event(refusal_attempted.writer());
                if(process_state.fail_refusal_publication) {
                    const rlimit limit{0, 0};
                    expect(setrlimit(RLIMIT_NOFILE, &limit) == 0, "child-only refusal fault failed");
                }
            }
        });
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) -> int {
            if(scenario == HelperExecutionScenario::RefuseAfterAuthorization) return fail_exec();
            if(scenario == HelperExecutionScenario::SignalBeforeObservation) {
                raise(SIGTERM);
                _exit(96);
            }
            const auto code = std::to_string(process_state.package_exit);
            const char* phase = scenario == HelperExecutionScenario::ExecUnobserved           ? "none"
                                : scenario == HelperExecutionScenario::ExecPostTransaction    ? "post"
                                : scenario == HelperExecutionScenario::SignalAfterObservation ? "signal"
                                                                                              : "pre";
            execl("/proc/self/exe", "source-artifact-install-trusted-transport-test", "--package-manager-phase-child",
                  runtime.c_str(), token.c_str(), phase, code.c_str(), static_cast<char*>(nullptr));
            _exit(97);
        });
        try {
            static_cast<void>(process_state.real_store->execute(token));
            _exit(98);
        } catch(const SourceArtifactInstallTrustedStateError& error) {
            _exit(error.refusal().reason == SourceArtifactInstallSealingFailure::ExecutableLaunchFailure
                      ? process_state.helper_exit
                      : 99);
        } catch(...) {
            _exit(100);
        }
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while(waited < 0 && errno == EINTR);
    expect(waited == child, "helper fixture was not reaped");
    pollfd notification{refusal_attempted.reader(), POLLIN, 0};
    expect(poll(&notification, 1, 0) >= 0, "refusal notification poll failed");
    process_state.refusal_publication_attempted = (notification.revents & POLLIN) != 0;
    if(process_state.refusal_publication_attempted) await_pipe_event(refusal_attempted.reader());
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    expect(WIFSIGNALED(status), "helper fixture outcome was neither exit nor signal");
    return 128 + WTERMSIG(status); // Preserve the actual process.cpp wait contract.
}

SourceArtifactInstallTrustedExecutionResult execute_scenario(
    PreparedInstallFixture& fixture,
    ProcessScenario scenario,
    char token_character) {
    process_state = {};
    process_state.scenario = scenario;
    return execute_source_artifact_install_trusted_transaction_for_test(
        fixture.install(), fixture.binding(),
        ArtifactInstallExecutionOptions{true},
        transaction_token(token_character));
}

void test_transport_observation_and_failure_matrix(
    const fs::path& archive) {
    PreparedInstallFixture complete_fixture(archive);
    const auto complete = execute_scenario(
        complete_fixture, ProcessScenario::Complete, '3');
    expect(
        complete.status() ==
                SourceArtifactInstallTrustedExecutionStatus::Complete &&
            complete.expectation().has_value() &&
            complete.observation().has_value() &&
            process_state.capture_count == 3 &&
            process_state.run_count == 1,
        "complete trusted transport did not close the process flow");
    const auto evidence = establish_source_artifact_install_receipt_evidence(
        *complete.expectation(), *complete.observation());
    const auto causal = project_source_artifact_install_causal_evidence(
        evidence);
    expect(
        evidence.completeness() ==
                SourceArtifactInstallReceiptEvidenceCompleteness::Complete &&
            causal.has_value() &&
            evidence.actual_install_set() ==
                std::vector<std::string>{
                    "moguet-source-transport-test", "solver-added"},
        "trusted transport did not produce closed causal evidence");

    expect(
        !complete.expectation()
                ->work_item.invocation_authority.has_value() &&
            !complete.observation()
                 ->work_item()
                 .invocation_authority.has_value(),
        "standalone transport minted cleanup invocation authority");
    SourceArtifactInstallReceiptExpectation wrong_work_item =
        *complete.expectation();
    ++wrong_work_item.work_item.work_item_index;
    const auto cross_work_item_evidence =
        establish_source_artifact_install_receipt_evidence(
            wrong_work_item, *complete.observation());
    expect(
        !project_source_artifact_install_causal_evidence(
             cross_work_item_evidence)
             .has_value(),
        "transport observation crossed work-item identity");

    process_state = {};
    const auto replay =
        execute_source_artifact_install_trusted_transaction_for_test(
            complete_fixture.install(), complete_fixture.binding(),
            ArtifactInstallExecutionOptions{true}, transaction_token('4'));
    expect(
        replay.status() ==
                SourceArtifactInstallTrustedExecutionStatus::InvalidRequest &&
            process_state.capture_count == 0 && process_state.run_count == 0,
        "consumed prepared transport capability was replayed");

    PreparedInstallFixture missing_fixture(archive, true);
    const auto missing = execute_scenario(
        missing_fixture, ProcessScenario::Missing, '5');
    const auto missing_evidence =
        establish_source_artifact_install_receipt_evidence(
            *missing.expectation(), *missing.observation());
    expect(
        missing.status() ==
                SourceArtifactInstallTrustedExecutionStatus::Missing &&
            missing_evidence.completeness() ==
                SourceArtifactInstallReceiptEvidenceCompleteness::Missing &&
            !project_source_artifact_install_causal_evidence(
                 missing_evidence)
                 .has_value(),
        "missing receipt became positive");

    PreparedInstallFixture failed_fixture(archive);
    const auto failed = execute_scenario(
        failed_fixture, ProcessScenario::PacmanFailure, '6');
    expect(
        failed.status() ==
                SourceArtifactInstallTrustedExecutionStatus::PacmanFailed &&
            failed.pacman_exit_status() == std::optional<int>{42} &&
            failed.observation()->transaction_ledger().transactions.front().command_outcome ==
                InvocationDependencyTransactionCommandOutcome::Failed &&
            !project_source_artifact_install_causal_evidence(
                 establish_source_artifact_install_receipt_evidence(
                     *failed.expectation(), *failed.observation()))
                 .has_value(),
        "failed transaction became positive");

    PreparedInstallFixture malformed_fixture(archive);
    const auto malformed = execute_scenario(
        malformed_fixture, ProcessScenario::MalformedReceipt, '7');
    expect(
        malformed.status() ==
                SourceArtifactInstallTrustedExecutionStatus::
                    MalformedReceipt &&
            malformed.observation()->transaction_ledger().transactions.front().receipt.state() ==
                PacmanTransactionReceiptState::Invalid,
        "malformed receipt was flattened to an empty receipt");

    PreparedInstallFixture invalid_fixture(archive);
    SourceArtifactInstallTrustedBinding invalid = invalid_fixture.binding();
    invalid.selected_artifacts[0].dependency_roles = {PackageRole::Root};
    process_state = {};
    const auto invalid_result =
        execute_source_artifact_install_trusted_transaction_for_test(
            invalid_fixture.install(), invalid,
            ArtifactInstallExecutionOptions{true}, transaction_token('8'));
    expect(
        invalid_result.status() ==
                SourceArtifactInstallTrustedExecutionStatus::InvalidRequest &&
            !invalid_result.expectation().has_value() &&
            process_state.capture_count == 0 && process_state.run_count == 0,
        "invalid role reached root preparation");
}

} // namespace

CapturedCommandResult capture_explicit_process_output_raw(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_error) {
    expect(!suppress_standard_error, "source transport suppressed helper errors");
    ++process_state.capture_count;
    const std::vector<std::string> arguments = helper_arguments(invocation);
    expect(!arguments.empty(), "source helper invocation is empty");

    if(arguments[0] == "prepare") {
        ++process_state.prepare_count;
        expect(
            invocation.standard_input_fd.has_value() &&
                invocation.stdout_capture_limit ==
                    SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES,
            "source prepare did not carry bounded sealed input");
        constexpr int seals =
            F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
        expect(
            (fcntl(*invocation.standard_input_fd, F_GET_SEALS) & seals) ==
                seals,
            "source prepare input was mutable");
        const auto parsed =
            parse_source_artifact_install_trusted_helper_arguments(arguments);
        const auto* helper =
            std::get_if<SourceArtifactInstallTrustedHelperInvocation>(&parsed);
        expect(helper != nullptr, "transport emitted invalid source prepare argv");
        SourceArtifactInstallRootPrepareRequest request{
            helper->transaction_token,
            helper->package_base,
            helper->directive,
            helper->needed,
            helper->no_confirm,
            helper->artifacts};
        expect(request.artifacts[0].archive_sha256 ==
                   xdg_generation_store_file_descriptor_sha256(*invocation.standard_input_fd,
                                                               request.artifacts[0].artifact_size, SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES),
               "expected digest did not originate from the exact sealed input");
        process_state.request = request;
        SourceArtifactInstallRootPrepareResponse response{
            request.transaction_token,
            source_artifact_install_hook_directory(request.transaction_token),
            {{request.artifacts[0].artifact_index,
              source_artifact_install_staged_artifact_path(
                  request.transaction_token, 0)}}};
        if(process_state.real_store)
            response = process_state.real_store->prepare(request, *invocation.standard_input_fd);
        return CapturedCommandResult{
            serialize_source_artifact_install_root_prepare_response(
                response, request),
            0,
            false};
    }

    if(arguments[0] == "execution-status") {
        if(process_state.real_store) {
            if(process_state.before_status) std::exchange(process_state.before_status, {})();
            process_state.private_evidence = snapshot_private_evidence(process_state.transaction);
            const auto observation = process_state.real_store->execution_status(arguments[1]);
            auto protocol = serialize_source_artifact_install_execution_observation(observation);
            switch(process_state.execution_status) {
                case ExecutionStatusScenario::QueryFailure: return {protocol, 1, false};
                case ExecutionStatusScenario::CaptureFailure: throw std::runtime_error("injected status capture failure");
                case ExecutionStatusScenario::Malformed: return {"malformed\n", 0, false};
                case ExecutionStatusScenario::Truncated: return {protocol.substr(0, protocol.size() - 5), 0, false};
                case ExecutionStatusScenario::CaptureLimit: return {protocol, 0, true};
                case ExecutionStatusScenario::OldSchema:
                case ExecutionStatusScenario::FutureSchema:
                    protocol[protocol.find('\t') + 1] =
                        process_state.execution_status == ExecutionStatusScenario::OldSchema ? '2' : '4';
                    return {protocol, 0, false};
                case ExecutionStatusScenario::Unauthorized:
                    return {serialize_source_artifact_install_execution_observation({arguments[1], false, std::nullopt}), 0, false};
                case ExecutionStatusScenario::Missing: return {"", 0, false};
                case ExecutionStatusScenario::WrongToken:
                    return {serialize_source_artifact_install_execution_observation({transaction_token('f'), true, std::nullopt}), 0, false};
                case ExecutionStatusScenario::Valid: return {protocol, 0, false};
            }
        }
        return CapturedCommandResult{serialize_source_artifact_install_execution_observation(
                                         {arguments[1], process_state.scenario != ProcessScenario::SealingFailure,
                                          process_state.scenario == ProcessScenario::SealingFailure
                                              ? std::optional<SourceArtifactInstallSealingRefusal>{{SourceArtifactInstallSealingFailure::StagedArtifactDigestMismatch}}
                                              : std::nullopt,
                                          process_state.scenario == ProcessScenario::SealingFailure
                                              ? SourceArtifactInstallExecutionEvidence::Unobserved
                                              : SourceArtifactInstallExecutionEvidence::PreTransaction}),
                                     0, false};
    }
    expect(arguments[0] == "consume", "unexpected source helper capture verb");
    ++process_state.consume_count;
    expect(
        invocation.stdout_capture_limit ==
            SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES,
        "source consume was not bounded");
    const std::string& token = arguments[1];
    if(process_state.real_store) return {process_state.real_store->consume(token), 0, false};
    switch(process_state.scenario) {
        case ProcessScenario::Complete:
            return CapturedCommandResult{
                serialize_source_artifact_install_root_receipt(
                    SourceArtifactInstallRootReceipt{
                        SourceArtifactInstallRootReceiptState::Complete,
                        token,
                        {"moguet-source-transport-test", "solver-added"}}),
                0,
                false};
        case ProcessScenario::Missing:
            return CapturedCommandResult{
                serialize_source_artifact_install_root_receipt(
                    SourceArtifactInstallRootReceipt{
                        SourceArtifactInstallRootReceiptState::Missing,
                        token,
                        {}}),
                0,
                false};
        case ProcessScenario::MalformedReceipt:
            return CapturedCommandResult{"malformed\n", 0, false};
        case ProcessScenario::ConsumeSealingFailure:
            return {serialize_source_artifact_install_execution_observation({token, false,
                                                                             SourceArtifactInstallSealingRefusal{
                                                                                 SourceArtifactInstallSealingFailure::StagedArtifactGenerationMismatch}}),
                    1, false};
        case ProcessScenario::PacmanFailure:
        case ProcessScenario::SealingFailure:
        case ProcessScenario::UnknownOutcome:
            throw std::logic_error("consume ran after pacman failure");
    }
    throw std::logic_error("unknown process scenario");
}

int run_explicit_process(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_output,
    bool suppress_standard_error) {
    expect(
        !suppress_standard_output && !suppress_standard_error,
        "source transport suppressed process output");
    const bool is_helper = invocation.arguments.size() >= 3 &&
                           invocation.arguments[1] ==
                               MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH;
    if(is_helper) {
        const auto arguments = helper_arguments(invocation);
        if(arguments[0] == "abort") {
            ++process_state.abort_count;
            if(process_state.real_store) process_state.real_store->abort(arguments[1]);
            return 0;
        }
        expect(arguments[0] == "execute", "unexpected source helper run verb");
    }
    expect(is_helper, "outer transport tried to run pacman directly");
    ++process_state.run_count;
    verify_pacman_invocation(invocation);
    if(process_state.real_store) {
        if(process_state.helper_execution != HelperExecutionScenario::InProcess) return run_helper_in_child();
        try {
            return process_state.real_store->execute(process_state.request->transaction_token);
        } catch(const SourceArtifactInstallTrustedStateError&) {
            return 42; // Intentionally identical to the known pacman failure.
        }
    }
    return process_state.scenario == ProcessScenario::PacmanFailure ||
                   process_state.scenario == ProcessScenario::SealingFailure
               ? 42
               : 0;
}

ExplicitProcessExecutionResult run_explicit_process_with_outcome(
    const ExplicitProcessInvocation& invocation, bool suppress_standard_output,
    bool suppress_standard_error) noexcept {
    try {
        const int status = run_explicit_process(invocation, suppress_standard_output, suppress_standard_error);
        if(process_state.scenario == ProcessScenario::UnknownOutcome)
            return {ExplicitProcessExecutionStatus::StartedOutcomeUnknown, std::nullopt};
        return {ExplicitProcessExecutionStatus::StartedKnownOutcome, status};
    } catch(...) {
        return {ExplicitProcessExecutionStatus::StartedOutcomeUnknown, std::nullopt};
    }
}

void test_transport_sealing_result(const fs::path& archive) {
    PreparedInstallFixture fixture(archive);
    const auto refusal = execute_scenario(fixture, ProcessScenario::SealingFailure, 'a');
    expect(refusal.status() == SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed &&
               refusal.sealing_failure() && refusal.sealing_failure()->reason == SourceArtifactInstallSealingFailure::StagedArtifactDigestMismatch &&
               !refusal.pacman_exit_status() && !refusal.operation_result() && process_state.abort_count == 1,
           "helper refusal was flattened into a package process result");
    PreparedInstallFixture consumed(archive);
    const auto drift = execute_scenario(consumed, ProcessScenario::ConsumeSealingFailure, 'b');
    expect(drift.status() == SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed &&
               drift.sealing_failure() && drift.operation_result() && drift.pacman_exit_status() == 0,
           "post-exec sealing failure overwrote successful operation or became Complete");
    PreparedInstallFixture unknown(archive);
    expect(execute_scenario(unknown, ProcessScenario::UnknownOutcome, 'c').status() ==
               SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown,
           "ambiguous process outcome became unattempted or Complete");
}

void test_unknown_status_evidence_retention(const fs::path& archive) {
    using Status = ExecutionStatusScenario;
    const std::vector<std::pair<Status, std::string>> scenarios{
        {Status::QueryFailure, "query failure"}, {Status::CaptureFailure, "capture failure"}, {Status::Malformed, "malformed"}, {Status::Truncated, "truncated"}, {Status::CaptureLimit, "capture limit"}, {Status::OldSchema, "old schema"}, {Status::FutureSchema, "future schema"}, {Status::Unauthorized, "unauthorized without refusal"}, {Status::Missing, "missing"}, {Status::WrongToken, "wrong token"}};
    for(const auto& [scenario, name] : scenarios) {
        PreparedInstallFixture fixture(archive);
        TemporaryDirectory runtime("moguet-unknown-private-evidence");
        auto store = open_source_store(runtime);
        const auto token = transaction_token('a');
        process_state = {};
        process_state.scenario = ProcessScenario::PacmanFailure;
        process_state.execution_status = scenario;
        process_state.real_store = &store;
        process_state.transaction = runtime.path() / "moguet/source-artifact-installs/active" / token;
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) {
            if(scenario == Status::CaptureFailure) throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::ExecutableLaunchFailure, "unobserved typed refusal", ENOENT);
            // Existing private hook evidence must also survive an ambiguous
            // outer observation; it is never promoted to an outer Complete.
            auto targets = input_text("moguet-source-transport-test\n");
            store.record(token, targets.get());
            return 42;
        });
        const auto result = execute_source_artifact_install_trusted_transaction_for_test(
            fixture.install(), fixture.binding(), ArtifactInstallExecutionOptions{true}, token);
        expect(result.status() == SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown &&
                   !result.operation_result() && !result.pacman_exit_status() &&
                   process_state.abort_count == 0 && process_state.run_count == 1 &&
                   process_state.prepare_count == 1 && process_state.consume_count == 0,
               name + ": unknown execution aborted, retried, or became a known outcome");
        expect(result.expectation() && result.observation(), name + ": diagnostic token/observation lost");
        const auto& transaction = result.observation()->transaction_ledger().transactions.front();
        const auto evidence = establish_source_artifact_install_receipt_evidence(*result.expectation(), *result.observation());
        expect(transaction.command_outcome == InvocationDependencyTransactionCommandOutcome::Unknown &&
                   evidence.completeness() != SourceArtifactInstallReceiptEvidenceCompleteness::Complete &&
                   !project_source_artifact_install_causal_evidence(evidence),
               name + ": unknown execution produced Complete authority");
        expect(!process_state.private_evidence.empty() &&
                   snapshot_private_evidence(process_state.transaction) == process_state.private_evidence,
               name + ": private evidence bytes or namespace generation changed");
        for(const auto leaf : {"prepared", "identity", "lifetime", "execution", "authorized", "artifacts", "hooks"})
            expect(fs::exists(process_state.transaction / leaf), name + ": missing retained evidence " + leaf);
        expect(fs::exists(process_state.transaction / (scenario == Status::CaptureFailure ? "refusal" : "receipt")),
               name + ": existing refusal or hook receipt evidence was deleted");
        expect(!fs::exists(runtime.path() / "moguet/source-artifact-installs/used" / token),
               name + ": active evidence became a used tombstone");
        const auto captures = process_state.capture_count;
        const auto replay = execute_source_artifact_install_trusted_transaction_for_test(
            fixture.install(), fixture.binding(), ArtifactInstallExecutionOptions{true}, transaction_token('b'));
        expect(replay.status() == SourceArtifactInstallTrustedExecutionStatus::InvalidRequest &&
                   process_state.capture_count == captures && process_state.run_count == 1 && process_state.abort_count == 0,
               name + ": ambiguous transaction capability permitted restart");
        process_state.real_store = nullptr;
        std::cout << "F-A02: " << name << " OutcomeUnknown / retained / abort=0 / retry=0 / no Complete PASS\n";
    }

    for(bool refusal : {true, false}) {
        PreparedInstallFixture fixture(archive);
        TemporaryDirectory runtime("moguet-known-execution-outcome");
        auto store = open_source_store(runtime);
        const auto token = transaction_token('c');
        process_state = {};
        process_state.real_store = &store;
        process_state.transaction = runtime.path() / "moguet/source-artifact-installs/active" / token;
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) -> int {
            if(refusal) throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::ExecutableLaunchFailure, "injected exec failure", ENOENT);
            store.observe_execution(token);
            return 42;
        });
        const auto result = execute_source_artifact_install_trusted_transaction_for_test(
            fixture.install(), fixture.binding(), ArtifactInstallExecutionOptions{true}, token);
        expect(process_state.run_count == 1 && process_state.abort_count == 1 &&
                   !fs::exists(process_state.transaction) &&
                   fs::is_empty(runtime.path() / "moguet/source-artifact-installs/used" / token),
               "known outcome lost safe post-exec cleanup");
        if(refusal) {
            expect(result.status() == SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed &&
                       result.sealing_failure() && result.sealing_failure()->reason == SourceArtifactInstallSealingFailure::ExecutableLaunchFailure &&
                       result.sealing_failure()->error_number == ENOENT && !result.pacman_exit_status(),
                   "known typed exec refusal was converted to Unknown or pacman failure");
        } else {
            expect(result.status() == SourceArtifactInstallTrustedExecutionStatus::PacmanFailed &&
                       result.pacman_exit_status() == 42 && !result.sealing_failure(),
                   "known pacman failure lost its actual exit code");
        }
        process_state.real_store = nullptr;
        std::cout << "F-A02: known " << (refusal ? "helper refusal" : "pacman failure")
                  << " with equal outer status 42; typed outcome / lease release / safe cleanup PASS\n";
    }
    set_source_artifact_install_trusted_exec_test_hook([](const auto&) { return 0; });
}

void assert_unknown_evidence_retained(const SourceArtifactInstallTrustedExecutionResult& result,
                                      const fs::path& transaction, const std::string& name) {
    expect(result.status() == SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown &&
               !result.pacman_exit_status() && !result.operation_result() && !result.sealing_failure() &&
               process_state.abort_count == 0 && process_state.consume_count == 0 &&
               process_state.run_count == 1 && process_state.prepare_count == 1,
           name + ": unproven execution became a known result, cleanup, or retry");
    expect(result.expectation() && result.observation(), name + ": unknown lost token context");
    const auto evidence = establish_source_artifact_install_receipt_evidence(*result.expectation(), *result.observation());
    expect(evidence.completeness() != SourceArtifactInstallReceiptEvidenceCompleteness::Complete &&
               !project_source_artifact_install_causal_evidence(evidence),
           name + ": unknown became Complete");
    expect(!process_state.private_evidence.empty() &&
               snapshot_private_evidence(transaction) == process_state.private_evidence &&
               !fs::exists(transaction.parent_path().parent_path() / "used" / transaction.filename()),
           name + ": private bytes/generation were changed or replaced by an empty tombstone");
}

void run_fr01_execution_case(const fs::path& archive, const std::string& name,
                             HelperExecutionScenario scenario, bool fail_publication, int package_exit, int helper_exit,
                             SourceArtifactInstallTrustedExecutionStatus expected) {
    const bool unknown = expected == SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown;
    PreparedInstallFixture fixture(archive, false, unknown);
    TemporaryDirectory runtime("moguet-fr01-execution");
    auto store = open_source_store(runtime);
    const auto token = transaction_token('a');
    const auto transaction = runtime.path() / "moguet/source-artifact-installs/active" / token;
    process_state = {};
    process_state.real_store = &store;
    process_state.transaction = transaction;
    process_state.helper_execution = scenario;
    process_state.fail_refusal_publication = fail_publication;
    process_state.package_exit = package_exit;
    process_state.helper_exit = helper_exit;
    const auto result = execute_source_artifact_install_trusted_transaction_for_test(
        fixture.install(), fixture.binding(), ArtifactInstallExecutionOptions{true}, token);
    expect(result.status() == expected, name + ": wrong execution classification");
    const bool refusal = scenario == HelperExecutionScenario::RefuseBeforeAuthorization ||
                         scenario == HelperExecutionScenario::RefuseAfterAuthorization;
    expect(process_state.refusal_publication_attempted == refusal,
           name + ": refusal injection did not reach the publication call");
    if(unknown) {
        assert_unknown_evidence_retained(result, transaction, name);
        expect(fs::exists(transaction / "artifacts/artifact-0.pkg.tar.zst.sig"), name + ": signature evidence was lost");
        if(fail_publication) expect(!fs::exists(transaction / "refusal"), name + ": refusal publication did not fail");
        const auto captures = process_state.capture_count;
        const auto replay = execute_source_artifact_install_trusted_transaction_for_test(
            fixture.install(), fixture.binding(), ArtifactInstallExecutionOptions{true}, transaction_token('b'));
        expect(replay.status() == SourceArtifactInstallTrustedExecutionStatus::InvalidRequest &&
                   process_state.capture_count == captures && process_state.run_count == 1,
               name + ": consumed unknown capability could restart");
    } else {
        expect(!fs::exists(transaction) && fs::is_empty(runtime.path() / "moguet/source-artifact-installs/used" / token),
               name + ": proven later cleanup remained blocked");
        if(refusal) {
            expect(result.sealing_failure() && result.sealing_failure()->reason == SourceArtifactInstallSealingFailure::ExecutableLaunchFailure &&
                       result.sealing_failure()->error_number == ENOENT && !result.pacman_exit_status() && process_state.abort_count == 1,
                   name + ": trusted helper refusal was flattened");
        } else {
            expect(process_state.private_evidence.contains("execution-observed") && !result.sealing_failure() &&
                       result.pacman_exit_status() == (scenario == HelperExecutionScenario::SignalAfterObservation ? 143 : package_exit),
                   name + ": package outcome lacked positive origin or lost its numeric status");
            if(package_exit == 0 && scenario != HelperExecutionScenario::SignalAfterObservation) {
                expect(result.operation_result() && result.operation_result()->is_success() && process_state.consume_count == 1 &&
                           process_state.abort_count == 0,
                       name + ": ordinary Install success regressed");
            } else
                expect(process_state.abort_count == 1 && !result.operation_result(), name + ": known package failure became success");
        }
    }
    process_state.real_store = nullptr;
    std::cout << name << ": " << (unknown ? "OutcomeUnknown / retain / abort=0 / consume=0 / retry=0 / no Complete" : refusal ? "ArtifactSealingFailed / typed refusal / safe cleanup"
                                                                                                                              : "positive hook origin / exact package outcome / safe cleanup")
              << " PASS\n";
}

void test_fr01_execution_matrix(const fs::path& archive) {
    using Scenario = HelperExecutionScenario;
    using Status = SourceArtifactInstallTrustedExecutionStatus;
    set_source_artifact_install_trusted_exec_test_hook([](const auto&) { return 0; });
    run_fr01_execution_case(archive, "F-R01-1 pre-auth refusal", Scenario::RefuseBeforeAuthorization, false, 1, 1, Status::ArtifactSealingFailed);
    run_fr01_execution_case(archive, "F-R01-2 pre-auth refusal create failure", Scenario::RefuseBeforeAuthorization, true, 1, 1, Status::OutcomeUnknown);
    run_fr01_execution_case(archive, "F-R01-3 post-auth refusal", Scenario::RefuseAfterAuthorization, false, 1, 1, Status::ArtifactSealingFailed);
    run_fr01_execution_case(archive, "F-R01-4 post-auth refusal create failure", Scenario::RefuseAfterAuthorization, true, 1, 1, Status::OutcomeUnknown);
    run_fr01_execution_case(archive, "F-R01-5 pre-positive signal", Scenario::SignalBeforeObservation, false, 1, 1, Status::OutcomeUnknown);
    run_fr01_execution_case(archive, "F-R01-6 positively observed exit 1", Scenario::ExecPreTransaction, false, 1, 1, Status::PacmanFailed);
    run_fr01_execution_case(archive, "F-R01-7 positively observed exit 42", Scenario::ExecPreTransaction, false, 42, 1, Status::PacmanFailed);
    run_fr01_execution_case(archive, "F-R01-8 same numeric helper refusal 42", Scenario::RefuseAfterAuthorization, false, 42, 42, Status::ArtifactSealingFailed);
    run_fr01_execution_case(archive, "F-R01 observed signal", Scenario::SignalAfterObservation, false, 42, 1, Status::PacmanFailed);
    run_fr01_execution_case(archive, "F-R01 actual exec without phase evidence", Scenario::ExecUnobserved, false, 42, 1, Status::OutcomeUnknown);
    run_fr01_execution_case(archive, "F-R01 zero without phase evidence", Scenario::ExecUnobserved, false, 0, 1, Status::OutcomeUnknown);
    run_fr01_execution_case(archive, "F-R01 ordinary observed Install", Scenario::ExecPreTransaction, false, 0, 1, Status::Complete);
    run_fr01_execution_case(archive, "F-R01 later PostTransaction evidence", Scenario::ExecPostTransaction, false, 42, 1, Status::PacmanFailed);
    run_fr01_execution_case(archive, "F-R01 existing PostTransaction success", Scenario::ExecPostTransaction, false, 0, 1, Status::Complete);
}

void test_fr01_evidence_failures(const fs::path& archive) {
    for(const std::string kind : {"missing", "malformed", "future", "wrong-token", "new-inode", "other-transaction", "symlink", "mode", "hardlink",
                                  "stage-generation", "lease-generation", "claim-missing", "unauthorized", "refusal",
                                  "old-claim-schema", "forged-authorization-phase"}) {
        PreparedInstallFixture fixture(archive, false, true);
        TemporaryDirectory runtime("moguet-fr01-marker");
        auto store = open_source_store(runtime);
        const auto token = transaction_token('a');
        const auto transaction = runtime.path() / "moguet/source-artifact-installs/active" / token;
        const auto marker = transaction / "execution-observed";
        process_state = {};
        process_state.real_store = &store;
        process_state.transaction = transaction;
        process_state.helper_execution = HelperExecutionScenario::ExecPreTransaction;
        process_state.before_status = [&] {
            expect(fs::exists(marker), "real exec/hook producer did not create the marker before corruption");
            if(kind == "missing")
                fs::remove(marker);
            else if(kind == "symlink") {
                const auto target = runtime.path() / "marker-copy";
                fs::copy_file(marker, target);
                fs::remove(marker);
                fs::create_symlink(target, marker);
            } else if(kind == "mode")
                fs::permissions(marker, fs::perms::group_write, fs::perm_options::add);
            else if(kind == "hardlink")
                fs::create_hard_link(marker, runtime.path() / "marker-alias");
            else if(kind == "malformed")
                write_fixture_bytes(marker, "partial\n");
            else if(kind == "future") {
                auto bytes = read_fixture_bytes(marker);
                bytes[bytes.find('\t') + 1] = '2';
                write_fixture_bytes(marker, bytes);
            } else if(kind == "wrong-token") {
                auto bytes = read_fixture_bytes(marker);
                bytes.replace(bytes.find(token), token.size(), transaction_token('b'));
                write_fixture_bytes(marker, bytes);
            } else if(kind == "new-inode" || kind == "stage-generation" || kind == "lease-generation") {
                const auto path = kind == "new-inode" ? marker : kind == "lease-generation" ? transaction / "lifetime"
                                                                                            : transaction / "artifacts/artifact-0.pkg.tar.zst";
                const auto replacement = runtime.path() / "replacement";
                write_fixture_bytes(replacement, read_fixture_bytes(path));
                fs::rename(replacement, path);
            } else if(kind == "other-transaction") {
                const auto other = transaction_token('b');
                auto request = root_request(other, archive);
                auto input = sealed_input_from_files({archive});
                static_cast<void>(store.prepare(request, input.get()));
                expect(store.execute(other) == 0, "other transaction fixture failed");
                store.observe_execution(other);
                fs::rename(transaction.parent_path() / other / "execution-observed", marker);
            } else if(kind == "claim-missing")
                fs::remove(transaction / "execution");
            else if(kind == "unauthorized")
                write_fixture_bytes(transaction / "authorized",
                                    serialize_source_artifact_install_execution_observation({token, false, std::nullopt}));
            else if(kind == "refusal")
                write_fixture_bytes(transaction / "refusal",
                                    serialize_source_artifact_install_execution_observation({token, true,
                                                                                             SourceArtifactInstallSealingRefusal{SourceArtifactInstallSealingFailure::ExecutableLaunchFailure, ENOENT}}));
            else if(kind == "old-claim-schema") {
                auto bytes = read_fixture_bytes(transaction / "execution");
                bytes[bytes.find('\t') + 1] = '2';
                write_fixture_bytes(transaction / "execution", bytes);
            } else if(kind == "forged-authorization-phase") {
                fs::remove(marker);
                write_fixture_bytes(transaction / "authorized", serialize_source_artifact_install_execution_observation(
                                                                    {token, true, std::nullopt, SourceArtifactInstallExecutionEvidence::PreTransaction}));
            }
        };
        const auto result = execute_source_artifact_install_trusted_transaction_for_test(
            fixture.install(), fixture.binding(), ArtifactInstallExecutionOptions{true}, token);
        assert_unknown_evidence_retained(result, transaction, kind);
        process_state.real_store = nullptr;
        std::cout << "F-R01-9/10 " << kind << ": Unknown / retained / no cleanup or Complete PASS\n";
    }
    for(const std::string version : {"1", "2", "4"}) {
        auto bytes = serialize_source_artifact_install_execution_observation(
            {transaction_token('a'), true, std::nullopt, SourceArtifactInstallExecutionEvidence::PreTransaction});
        bytes.replace(bytes.find('\t') + 1, 1, version);
        expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(parse_source_artifact_install_execution_observation(bytes)),
               "old/future execution status schema was adopted");
    }
    for(const auto evidence : {SourceArtifactInstallExecutionEvidence::PreTransaction, SourceArtifactInstallExecutionEvidence::PostTransaction}) {
        const auto valid = serialize_source_artifact_install_execution_observation({transaction_token('a'), true, std::nullopt, evidence});
        expect(std::get<SourceArtifactInstallExecutionObservation>(parse_source_artifact_install_execution_observation(valid)).execution_evidence == evidence,
               "positive execution phase did not roundtrip");
        auto contradictory = valid;
        contradictory.replace(contradictory.find("AUTHORIZED\t1"), std::string("AUTHORIZED\t1").size(), "AUTHORIZED\t0");
        expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(parse_source_artifact_install_execution_observation(contradictory)),
               "unauthorized positive execution parsed");
    }
    {
        SealedStageFixture fixture;
        expect_sealing_refusal([&] { fixture.store.observe_execution(fixture.token); },
                               SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch);
        expect(!fs::exists(fixture.transaction / "execution-observed"), "observation before authorization minted a marker");
        expect(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(
                   parse_source_artifact_install_trusted_helper_arguments({"observe-execution", fixture.token, "/tmp/marker"})),
               "caller could select an execution marker path");
        expect(fixture.store.execute(fixture.token) == 0, "observation lock fixture execution failed");
        OrderedProcess observer([&](int ready, int resume) {
            set_source_artifact_install_trusted_state_test_hook([&](auto event, int, const auto&) {
                if(event == SourceArtifactInstallTrustedStateTestEvent::AfterArtifactMetadataValidation) {
                    send_pipe_event(ready);
                    await_pipe_event(resume);
                }
            });
            fixture.store.observe_execution(fixture.token);
        });
        observer.await_ready();
        expect_lifetime_busy([&] { fixture.store.abort(fixture.token); });
        expect_lifetime_busy([&] { static_cast<void>(fixture.store.consume(fixture.token)); });
        observer.resume();
        observer.finish();
        expect(fixture.store.execution_status(fixture.token).execution_evidence == SourceArtifactInstallExecutionEvidence::PreTransaction,
               "completed observer did not publish its proven phase");
        expect_failure([&] { fixture.store.observe_execution(fixture.token); }, "observation marker was replayed");
        fixture.store.abort(fixture.token);
    }
    std::cout << "F-R01 observer authorization/path/lifetime/replay guards PASS\n";
}

void test_exact_receipt_helper_matrix() {
    InstalledRecordObservationTestHooks observation_hooks;
    TemporaryDirectory absent_world("moguet-exact-no-database");
    observation_hooks.database_path = (absent_world.path() / "missing-db").string();
    observation_hooks.expected_owner = geteuid();
    set_installed_record_observation_test_hooks(observation_hooks);
    for(const auto& scenario : {"install", "upgrade", "missing", "duplicate", "conflict", "partial",
                                "recreated", "wrong-token", "wrong-stage", "old-protocol", "anchor-partial", "anchor-recreated"}) {
        SealedStageFixture fixture(false, true);
        expect(fixture.store.execute(fixture.token) == 0, "exact execute failed");
        fixture.store.observe_execution(fixture.token);
        const std::string selected = "moguet-source-transport-test\n";
        const auto record = [&](bool upgrade) {
            auto targets = input_text(selected);
            if(upgrade)
                fixture.store.record_upgrade(fixture.token, targets.get());
            else
                fixture.store.record_install(fixture.token, targets.get());
        };
        const std::string mode(scenario);
        if(mode != "missing" && mode != "partial") record(mode == "upgrade");
        const auto exact = fixture.transaction / "exact";
        if(mode == "duplicate" || mode == "conflict")
            expect_failure([&] { record(mode == "conflict"); }, "duplicate/conflicting hook was accepted");
        if(mode == "partial") write_fixture_bytes(exact / "install.partial", "partial\n");
        if(mode == "anchor-partial") write_fixture_bytes(exact / "install-anchor.partial", "partial\n");
        if(mode == "recreated" || mode == "anchor-recreated") {
            const auto target = exact / (mode == "recreated" ? "install" : "install-anchor");
            const auto replacement = fixture.runtime.path() / "replacement";
            write_fixture_bytes(replacement, read_fixture_bytes(target));
            fs::rename(replacement, target);
        }
        if(mode == "wrong-token" || mode == "wrong-stage" || mode == "old-protocol") {
            auto bytes = read_fixture_bytes(exact / "install");
            const auto old = mode == "wrong-token" ? fixture.token : mode == "wrong-stage" ? fixture.staged_identity
                                                                                           : "PRIVATE-RECORD\t1";
            const auto replacement = mode == "old-protocol" ? "PRIVATE-RECORD\t0" : std::string(64, 'b');
            bytes.replace(bytes.find(old), old.size(), replacement);
            write_fixture_bytes(exact / "install", bytes);
        }
        expect(fixture.store.execution_status(fixture.token).execution_evidence == SourceArtifactInstallExecutionEvidence::PreTransaction,
               "receipt/anchor failure erased the independent execution witness");
        const auto root = parse_exact_artifact_root_evidence(fixture.store.consume_exact(fixture.token), fixture.request, fixture.staged_identity);
        expect(std::holds_alternative<ExactArtifactRootEvidence>(root), "exact helper returned invalid framing");
        const auto& evidence = std::get<ExactArtifactRootEvidence>(root);
        const auto joined = join_exact_artifact_operation_fragments(evidence.installs, evidence.upgrades, fixture.request, fixture.staged_identity);
        if(mode == "install" || mode == "upgrade" || mode.starts_with("anchor-")) {
            const auto* records = std::get_if<ExactArtifactOperationRecords>(&joined);
            expect(records && records->size() == 1 && records->front().artifact.artifact_index == 4, "actual operation receipt was lost");
            expect(records->front().operation == (mode == "upgrade" ? ExactArtifactTransactionOperation::Upgrade : ExactArtifactTransactionOperation::Install),
                   "fixed operation kind was changed");
            expect(std::holds_alternative<InstalledRecordObservationIssue>(evidence.world), "unavailable fixture world became proof authority");
        } else {
            expect(std::holds_alternative<ExactArtifactReceiptIssue>(joined), "invalid/missing operation receipt was completed");
            expect(std::get<ExactArtifactReceiptIssue>(joined) == (mode == "missing" ? ExactArtifactReceiptIssue::Missing : ExactArtifactReceiptIssue::Invalid),
                   "exact fragment failure classification changed");
        }
        expect_failure([&] { static_cast<void>(fixture.store.consume_exact(fixture.token)); }, "exact receipt was consumed twice");
    }
    {
        SealedStageFixture legacy;
        expect(legacy.store.execute(legacy.token) == 0, "legacy execute failed");
        legacy.store.observe_execution(legacy.token);
        auto targets = input_text("moguet-source-transport-test\n");
        expect_failure([&] { legacy.store.record_upgrade(legacy.token, targets.get()); }, "exact Upgrade hook adopted legacy cleanup purpose");
        expect_failure([&] { static_cast<void>(legacy.store.consume_exact(legacy.token)); }, "exact consumption adopted legacy purpose");
        expect(std::get<SourceArtifactInstallRootReceipt>(parse_source_artifact_install_root_receipt(legacy.store.consume(legacy.token))).state ==
                   SourceArtifactInstallRootReceiptState::Missing,
               "Upgrade entered legacy cleanup receipt");
    }
    {
        SealedStageFixture exact(false, true);
        expect(exact.store.execute(exact.token) == 0, "exact execute failed");
        exact.store.observe_execution(exact.token);
        auto targets = input_text("moguet-source-transport-test\n");
        expect_failure([&] { exact.store.record(exact.token, targets.get()); }, "legacy Install hook adopted exact purpose");
        expect_failure([&] { static_cast<void>(exact.store.consume(exact.token)); }, "legacy consumption adopted exact purpose");
        exact.store.abort(exact.token);
    }
    {
        ActualArchiveFixture archives;
        const auto first = archives.create_archive("moguet-source-transport-test", "1-1", "moguet-source-transport-base", "any");
        const auto second = archives.create_archive("moguet-source-transport-other", "1-1", "moguet-source-transport-base", "any");
        TemporaryDirectory runtime("moguet-exact-mixed");
        auto store = open_source_store(runtime);
        auto request = root_request(transaction_token('a'), first);
        auto other = request.artifacts.front();
        request.artifacts.front().artifact_index = 1;
        other.artifact_index = 4;
        other.package_name = "moguet-source-transport-other";
        other.artifact_size = fixture_file_size(second);
        other.archive_sha256 = fixture_sha256(second);
        request.artifacts.push_back(other);
        for(auto& artifact : request.artifacts)
            artifact.raw_mtree_sha256 = std::string(64, 'd');
        request.purpose = SourceArtifactInstallTrustedPurpose::ExactInstalledBinding;
        auto input = sealed_input_from_files({first, second});
        const auto prepared = store.prepare(request, input.get());
        expect(store.execute(request.transaction_token) == 0, "mixed exact execute failed");
        store.observe_execution(request.transaction_token);
        auto install = input_text("moguet-source-transport-test\n");
        auto upgrade = input_text("moguet-source-transport-other\n");
        store.record_upgrade(request.transaction_token, upgrade.get());
        store.record_install(request.transaction_token, install.get());
        const auto root = std::get<ExactArtifactRootEvidence>(parse_exact_artifact_root_evidence(store.consume_exact(request.transaction_token), request, prepared.staged_identity_sha256));
        const auto records = std::get<ExactArtifactOperationRecords>(join_exact_artifact_operation_fragments(root.installs, root.upgrades, request, prepared.staged_identity_sha256));
        expect(records.size() == 2 && records[0].artifact.artifact_index == 1 && records[1].artifact.artifact_index == 4 &&
                   records[0].operation == ExactArtifactTransactionOperation::Install && records[1].operation == ExactArtifactTransactionOperation::Upgrade,
               "mixed helper receipt lost canonical index/operation mapping");
    }
    set_installed_record_observation_test_hooks({});
    std::cout << "S5-B exact operation helper protocol/publication/purpose matrix PASS\n";
}

int main(int argc, char* argv[]) {
    try {
        if(argc > 1 && std::string(argv[1]) == "--lease-exec-child") return lease_exec_child(argc, argv);
        if(argc > 1 && std::string(argv[1]) == "--package-manager-phase-child") return package_manager_phase_child(argc, argv);
        TemporaryCacheHome cache_home;
        set_source_artifact_install_trusted_exec_test_hook([](const auto&) { return 0; });
        if(argc == 2) {
            const std::string mode = argv[1];
            if(mode == "--exact-receipt") {
                test_exact_receipt_helper_matrix();
                return 0;
            }
            if(mode == "--cleanup-first-abort" || mode == "--cleanup-first-consume") {
                test_cleanup_first_ordering(mode == "--cleanup-first-consume");
                return 0;
            }
            if(mode == "--duplicate-protocol") {
                test_protocol_is_fixed_owner_and_closed();
                return 0;
            }
            if(mode == "--unknown-status") {
                ActualArchiveFixture archives;
                const auto archive = archives.create_archive("moguet-source-transport-test", "1-1", "moguet-source-transport-base", "any");
                test_unknown_status_evidence_retention(archive);
                return 0;
            }
            if(mode == "--fr01") {
                ActualArchiveFixture archives;
                const auto archive = archives.create_archive("moguet-source-transport-test", "1-1", "moguet-source-transport-base", "any");
                test_fr01_execution_matrix(archive);
                test_fr01_evidence_failures(archive);
                return 0;
            }
            throw std::runtime_error("unknown regression selection");
        }
        test_protocol_is_fixed_owner_and_closed();
        test_exact_receipt_helper_matrix();
        test_lifetime_races();
        test_state_staging_receipt_replay_and_owner_isolation();
        test_sealing_state_regressions();
        test_sealing_protocol_regressions();
        ActualArchiveFixture archives;
        const fs::path archive = archives.create_archive(
            "moguet-source-transport-test", "1-1",
            "moguet-source-transport-base", "any");
        test_transport_observation_and_failure_matrix(archive);
        test_transport_sealing_result(archive);
        test_unknown_status_evidence_retention(archive);
        test_fr01_execution_matrix(archive);
        test_fr01_evidence_failures(archive);
    } catch(const std::exception& error) {
        std::cerr << "source artifact trusted transport test failed: "
                  << error.what() << '\n';
        return 1;
    }
    return 0;
}
