#include "git_remote_revision_observer.hpp"

#include "devel_package_classification.hpp"
#include "process.hpp"
#include "source_entry_parser.hpp"
#include "srcinfo_source_metadata.hpp"
#include "trusted_git_process_policy.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;

static_assert(!std::is_default_constructible_v<
              AuthorityApprovedGitSourceIdentity>);
static_assert(!std::is_constructible_v<
              AuthorityApprovedGitSourceIdentity,
              VcsSourceIdentity>);
static_assert(!std::is_constructible_v<
              AuthorityApprovedGitSourceIdentity,
              ParsedSourceEntry>);
static_assert(!std::is_constructible_v<
              AuthorityApprovedGitSourceIdentity,
              ParsedSrcinfoSourceMetadata>);
static_assert(!std::is_constructible_v<
              AuthorityApprovedGitSourceIdentity,
              TrustedDevelSourceMetadata>);
static_assert(!std::is_default_constructible_v<ValidatedHttpsGitRemote>);
static_assert(!std::is_default_constructible_v<ValidatedExactGitBranch>);
static_assert(!std::is_constructible_v<
              ValidatedExactGitBranch,
              std::string>);
static_assert(!std::is_default_constructible_v<
              ValidatedGitRemoteSelector>);
static_assert(!std::is_default_constructible_v<
              GitRemoteRevisionObservationKey>);
static_assert(!std::is_default_constructible_v<
              ValidatedGitRemoteRevisionRequest>);
static_assert(!std::is_constructible_v<
              ValidatedGitRemoteRevisionRequest,
              ParsedSourceEntry>);
static_assert(!std::is_constructible_v<
              ValidatedGitRemoteRevisionRequest,
              ParsedSrcinfoSourceMetadata>);
static_assert(!std::is_constructible_v<
              ValidatedGitRemoteRevisionRequest,
              VcsSourceIdentity>);
static_assert(!std::is_constructible_v<
              ValidatedGitRemoteRevisionRequest,
              TrustedDevelSourceMetadata>);
static_assert(!std::is_convertible_v<
              ParsedSourceEntry,
              ValidatedGitRemoteRevisionRequest>);
static_assert(!std::is_convertible_v<
              ParsedSrcinfoSourceMetadata,
              ValidatedGitRemoteRevisionRequest>);
static_assert(!std::is_convertible_v<
              VcsSourceIdentity,
              ValidatedGitRemoteRevisionRequest>);
static_assert(!std::is_convertible_v<
              SourceAwarePackageIdentity,
              UpstreamGitRevision>);
static_assert(!std::is_constructible_v<
              UpstreamGitRevision,
              const SourceAwarePackageIdentity&>);
static_assert(!std::is_constructible_v<
              ValidatedGitRemoteRevisionRequest,
              const SourceAwarePackageIdentity&>);
static_assert(std::variant_size_v<
                  GitRemoteRevisionObservationResult> == 8);
static_assert(std::variant_size_v<decltype(std::declval<GitRemoteRevisionProcessFailure>().cause)> ==
              3);
static_assert(std::variant_size_v<ExactGitBranchValidationResult> == 3);

constexpr std::string_view SHA1 =
    "0123456789abcdef0123456789abcdef01234567";
constexpr std::string_view SHA256 =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";
constexpr std::string_view PROCESS_FIXTURE_MARKER =
    "--git-remote-revision-observer-process-fixture";

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

bool write_all(int descriptor, std::string_view bytes) {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const ssize_t written = write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if(written == -1 && errno == EINTR) continue;
        if(written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

void ignore_signal(int signal_number) {
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    require(sigemptyset(&action.sa_mask) == 0,
            "Failed to initialize observer fixture signal mask");
    require(sigaction(signal_number, &action, nullptr) == 0,
            "Failed to ignore observer fixture signal");
}

class OwnedDescriptor final {
public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

    ~OwnedDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

class TemporaryTree final {
public:
    TemporaryTree() {
        std::string path_template =
            "/tmp/moguet-git-observer-policy-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        require(created != nullptr,
                "Failed to create observer policy fixture");
        path_ = created;
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    ~TemporaryTree() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

class ScopedEnvironment final {
public:
    explicit ScopedEnvironment(
        const std::vector<std::string>& variable_names) {
        for(const std::string& name : variable_names) {
            const char* value = std::getenv(name.c_str());
            saved_.push_back(
                SavedEnvironment{name,
                                 value == nullptr
                                     ? std::nullopt
                                     : std::optional<std::string>(value)});
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    ~ScopedEnvironment() noexcept {
        for(const SavedEnvironment& saved : saved_) {
            if(saved.value.has_value()) {
                static_cast<void>(
                    setenv(saved.name.c_str(), saved.value->c_str(), 1));
            } else {
                static_cast<void>(unsetenv(saved.name.c_str()));
            }
        }
    }

    void set(const std::string& name, const std::string& value) {
        require(setenv(name.c_str(), value.c_str(), 1) == 0,
                "Failed to set observer environment fixture " + name);
    }

    void unset(const std::string& name) {
        require(unsetenv(name.c_str()) == 0,
                "Failed to unset observer environment fixture " + name);
    }

private:
    struct SavedEnvironment {
        std::string name;
        std::optional<std::string> value;
    };

    std::vector<SavedEnvironment> saved_;
};

template <typename Expected>
const Expected& expect_branch_validation_result(
    const ExactGitBranchValidationResult& result,
    std::string_view context) {
    const Expected* expected = std::get_if<Expected>(&result);
    if(expected == nullptr) {
        throw std::runtime_error(
            std::string(context) + ": unexpected branch result arm " +
            std::to_string(result.index()));
    }
    return *expected;
}

template <typename Function>
void expect_invalid_argument(Function&& function, const std::string& context) {
    try {
        std::forward<Function>(function)();
    } catch(const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(context + ": expected std::invalid_argument");
}

template <typename Expected>
const Expected& expect_result(
    const GitRemoteRevisionObservationResult& result,
    std::string_view context) {
    const Expected* expected = std::get_if<Expected>(&result);
    if(expected == nullptr) {
        throw std::runtime_error(
            std::string(context) + ": unexpected result arm " +
            std::to_string(result.index()));
    }
    return *expected;
}

GitRemoteRevisionObservationKey default_key(std::string_view remote) {
    return GitRemoteRevisionObservationKey::make(
        ValidatedHttpsGitRemote::make(remote),
        ValidatedGitRemoteSelector::default_head());
}

GitRemoteRevisionObservationKey exact_key(
    std::string_view remote, std::string branch_name) {
    return GitRemoteRevisionObservationKey::make(
        ValidatedHttpsGitRemote::make(remote),
        ValidatedGitRemoteSelector::exact_branch(
            make_validated_exact_git_branch_fixture_for_test(
                std::move(branch_name))));
}

ValidatedGitRemoteRevisionRequest default_request(
    std::string remote = "https://example.com/repo.git") {
    VcsSourceIdentity source = VcsSourceIdentity::make(
        VcsKind::Git,
        remote,
        VcsSelector::default_head());
    return ValidatedGitRemoteRevisionRequest::make(
        make_authority_approved_git_source_identity_fixture_for_test(
            std::move(source)),
        default_key(remote));
}

ValidatedGitRemoteRevisionRequest exact_request(
    std::string branch_name = "exact",
    std::string remote = "https://example.com/repo.git") {
    VcsSourceIdentity source = VcsSourceIdentity::make(
        VcsKind::Git,
        remote,
        VcsSelector::branch(branch_name));
    return ValidatedGitRemoteRevisionRequest::make(
        make_authority_approved_git_source_identity_fixture_for_test(
            std::move(source)),
        exact_key(remote, std::move(branch_name)));
}

ValidatedGitRemoteRevisionRequest production_exact_request(
    std::string remote,
    std::string branch_name) {
    const ExactGitBranchValidationResult validation =
        validate_exact_git_branch(branch_name);
    const auto* validated =
        std::get_if<ValidatedExactGitBranch>(&validation);
    require(validated != nullptr,
            "Production exact branch validation failed in fixture driver");

    VcsSourceIdentity source = VcsSourceIdentity::make(
        VcsKind::Git, remote, VcsSelector::branch(branch_name));
    return ValidatedGitRemoteRevisionRequest::make(
        make_authority_approved_git_source_identity_fixture_for_test(
            std::move(source)),
        GitRemoteRevisionObservationKey::make(
            ValidatedHttpsGitRemote::make(remote),
            ValidatedGitRemoteSelector::exact_branch(*validated)));
}

GitRemoteRevisionObservationResult run_process_fixture(
    const fs::path& executable,
    std::string mode,
    std::chrono::milliseconds hard_timeout = 2s,
    std::chrono::milliseconds termination_grace = 150ms) {
    return observe_git_remote_revision_process_fixture_for_test(
        default_request(),
        executable.string(),
        {std::string(PROCESS_FIXTURE_MARKER), std::move(mode)},
        hard_timeout,
        termination_grace);
}

std::string oid_record(std::string_view oid, std::string_view ref) {
    return std::string(oid) + '\t' + std::string(ref) + '\n';
}

std::string symref_record(
    std::string_view target, std::string_view ref = "HEAD") {
    return "ref: " + std::string(target) + '\t' + std::string(ref) + '\n';
}

int run_observer_process_fixture_child(int argc, char* argv[]) {
    require(argc == 3, "Observer process fixture requires one mode");
    const std::string_view mode(argv[2]);
    if(mode == "observed") {
        return write_all(STDOUT_FILENO, oid_record(SHA1, "HEAD")) ? 0 : 125;
    }
    if(mode == "bad-oid") {
        return write_all(STDOUT_FILENO, "bad-oid\tHEAD\n") ? 0 : 125;
    }
    if(mode == "wrong-ref") {
        return write_all(
                   STDOUT_FILENO,
                   oid_record(SHA1, "refs/heads/wrong"))
                   ? 0
                   : 125;
    }
    if(mode == "duplicate") {
        const std::string output =
            oid_record(SHA1, "HEAD") + oid_record(SHA1, "HEAD");
        return write_all(STDOUT_FILENO, output) ? 0 : 125;
    }
    if(mode == "extra-ref") {
        const std::string output =
            oid_record(SHA1, "HEAD") +
            oid_record(SHA1, "refs/archive/HEAD");
        return write_all(STDOUT_FILENO, output) ? 0 : 125;
    }
    if(mode == "control-byte") {
        std::string output = std::string(SHA1) + "\tHE";
        output.push_back('\x01');
        output += "AD\n";
        return write_all(STDOUT_FILENO, output) ? 0 : 125;
    }
    if(mode == "partial-output") {
        return write_all(
                   STDOUT_FILENO, std::string(SHA1) + "\tHEAD")
                   ? 0
                   : 125;
    }
    if(mode == "status-2-empty") return 2;
    if(mode == "status-2-output") {
        require(write_all(STDOUT_FILENO, oid_record(SHA1, "HEAD")),
                "Failed to write status-2 observer fixture");
        return 2;
    }
    if(mode == "git-exit") return 128;
    if(mode == "signaled") {
        raise(SIGTERM);
        return 125;
    }
    if(mode == "timeout") {
        ignore_signal(SIGTERM);
        while(true)
            pause();
    }
    if(mode == "capture-overflow") {
        ignore_signal(SIGTERM);
        require(
            write_all(
                STDOUT_FILENO,
                std::string(
                    GIT_REMOTE_OBSERVER_STDOUT_CAPTURE_LIMIT + 1U,
                    'x')),
            "Failed to write observer overflow fixture");
        while(true)
            pause();
    }
    throw std::runtime_error("Unknown observer process fixture mode");
}

ObservedGitRemoteRevision expect_observed(
    const ValidatedGitRemoteRevisionRequest& request,
    std::string_view output,
    std::string_view expected_oid,
    const std::string& context) {
    const GitRemoteRevisionObservationResult result =
        parse_git_remote_revision_observation(request, 0, output);
    const auto& observed =
        expect_result<ObservedGitRemoteRevision>(result, context);
    require(observed.request() == request,
            context + ": request identity was not retained");
    require(observed.revision().source() == request.source().source(),
            context + ": source identity was not retained");
    require(observed.revision().value().git_commit() != nullptr &&
                *observed.revision().value().git_commit() == expected_oid,
            context + ": observed OID differs");
    return observed;
}

void expect_malformed(
    const ValidatedGitRemoteRevisionRequest& request,
    int exit_status,
    std::string_view output,
    GitRemoteRevisionMalformedOutputReason reason,
    const std::string& context) {
    const auto result = parse_git_remote_revision_observation(
        request, exit_status, output);
    const auto& malformed_result =
        expect_result<GitRemoteRevisionMalformedOutput>(result, context);
    require(malformed_result.key == request.key(),
            context + ": observation key differs");
    require(malformed_result.reason == reason,
            context + ": malformed reason differs");
}

void expect_ambiguous(
    const ValidatedGitRemoteRevisionRequest& request,
    std::string_view output,
    GitRemoteRevisionAmbiguousOutputReason reason,
    std::size_t record_count,
    const std::string& context) {
    const auto result =
        parse_git_remote_revision_observation(request, 0, output);
    const auto& ambiguous_result =
        expect_result<GitRemoteRevisionAmbiguousOutput>(result, context);
    require(ambiguous_result.key == request.key(),
            context + ": observation key differs");
    require(ambiguous_result.reason == reason &&
                ambiguous_result.record_count == record_count,
            context + ": ambiguity classification differs");
}

void test_https_remote_canonicalization() {
    const auto canonical = ValidatedHttpsGitRemote::make(
        "https://example.com/repo.git");
    require(canonical.canonical_url() ==
                "https://example.com/repo.git",
            "Canonical HTTPS remote changed unexpectedly.");

    const auto normalized = ValidatedHttpsGitRemote::make(
        "HTTPS://EXAMPLE.COM:443/a/../repo.git");
    require(normalized == canonical,
            "Scheme, host, default port, or dot path was not canonicalized.");
    require(ValidatedHttpsGitRemote::make(
                "https://example.com/a/./repo.git")
                    .canonical_url() ==
                "https://example.com/a/repo.git",
            "Single-dot path component behavior differs.");
    require(ValidatedHttpsGitRemote::make(
                "https://example.com/%2e/repo.git") ==
                    canonical &&
                ValidatedHttpsGitRemote::make(
                    "https://example.com/%2E%2E/a/../repo.git") ==
                    canonical,
            "Percent-encoded dot component behavior differs.");
    require(ValidatedHttpsGitRemote::make(
                "https://example.com/%72epo.git")
                    .canonical_url() ==
                "https://example.com/%72epo.git",
            "Non-dot percent encoding was rewritten heuristically.");
    require(ValidatedHttpsGitRemote::make("https://example.com") ==
                ValidatedHttpsGitRemote::make("https://example.com/"),
            "Empty path and slash canonical forms differ.");
    require(ValidatedHttpsGitRemote::make(
                "https://example.com:444/repo.git")
                    .canonical_url() ==
                "https://example.com:444/repo.git",
            "Non-default HTTPS port was discarded.");
    require(ValidatedHttpsGitRemote::make(
                "https://[2001:DB8::1]/repo.git")
                    .canonical_url() ==
                "https://[2001:db8::1]/repo.git",
            "IPv6 literal canonical form differs.");
    require(ValidatedHttpsGitRemote::make(
                "HTTPS://XN--BCHER-KVA.EXAMPLE/repo.git")
                    .canonical_url() ==
                "https://xn--bcher-kva.example/repo.git",
            "ASCII punycode host canonical form differs.");

    const std::string unicode_idn =
        std::string("https://b") + "\xc3\xbc" +
        "cher.example/repo.git";
    expect_invalid_argument(
        [&unicode_idn] {
            static_cast<void>(ValidatedHttpsGitRemote::make(unicode_idn));
        },
        "Raw Unicode IDN host");

    std::string maximum = "https://example.com/";
    maximum.append(
        VALIDATED_HTTPS_GIT_REMOTE_MAX_INPUT_BYTES - maximum.size(),
        'a');
    require(ValidatedHttpsGitRemote::make(maximum).canonical_url() == maximum,
            "8 KiB URL boundary was not accepted.");

    std::string oversized = maximum;
    oversized.push_back('a');
    expect_invalid_argument(
        [&oversized] {
            static_cast<void>(ValidatedHttpsGitRemote::make(oversized));
        },
        "Oversized HTTPS remote");
}

void test_https_remote_rejections() {
    for(const std::string remote : {
            "https://user@example.com/repo.git",
            "https://user:password@example.com/repo.git",
            "https://:password@example.com/repo.git",
            "https://example.com/repo.git?",
            "https://example.com/repo.git?q=1",
            "https://example.com/repo.git#",
            "https://example.com/repo.git#fragment",
            "http://example.com/repo.git",
            "file:///tmp/repo.git",
            "ssh://example.com/repo.git",
            "https://[fe80::1%25eth0]/repo.git",
            "git@example.com:repo.git",
            "git+https://example.com/repo.git",
            "ext::helper command",
            "https://",
            "https:///repo.git"}) {
        expect_invalid_argument(
            [&remote] {
                static_cast<void>(ValidatedHttpsGitRemote::make(remote));
            },
            "Rejected remote " + remote);
    }

    for(char forbidden : {' ', '\t', '\n', '\r', '\f', '\v', '\x01',
                          '\x7f'}) {
        std::string remote = "https://example.com/repo";
        remote.push_back(forbidden);
        remote += ".git";
        expect_invalid_argument(
            [&remote] {
                static_cast<void>(ValidatedHttpsGitRemote::make(remote));
            },
            "Remote with forbidden byte");
    }

    std::string nul = "https://example.com/repo";
    nul.push_back('\0');
    nul += ".git";
    expect_invalid_argument(
        [&nul] {
            static_cast<void>(ValidatedHttpsGitRemote::make(nul));
        },
        "Remote with embedded NUL");
}

void test_selector_key_and_request_model() {
    const auto default_selector =
        ValidatedGitRemoteSelector::default_head();
    require(default_selector.kind() ==
                    ValidatedGitRemoteSelectorKind::DefaultHead &&
                default_selector.exact_branch() == nullptr,
            "Default HEAD selector gained a string payload.");

    const auto exact_selector = ValidatedGitRemoteSelector::exact_branch(
        make_validated_exact_git_branch_fixture_for_test("feature/exact"));
    require(exact_selector.kind() ==
                    ValidatedGitRemoteSelectorKind::ExactBranch &&
                exact_selector.exact_branch() != nullptr &&
                exact_selector.exact_branch()->name() == "feature/exact",
            "Exact branch selector lost its closed payload.");

    std::string maximum_branch(
        VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES, 'a');
    require(make_validated_exact_git_branch_fixture_for_test(maximum_branch)
                    .name() == maximum_branch,
            "4 KiB exact branch fixture boundary was not accepted.");
    expect_invalid_argument(
        [] {
            static_cast<void>(
                make_validated_exact_git_branch_fixture_for_test(""));
        },
        "Empty exact branch fixture");
    expect_invalid_argument(
        [] {
            static_cast<void>(
                make_validated_exact_git_branch_fixture_for_test(
                    std::string(
                        VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES + 1,
                        'a')));
        },
        "Oversized exact branch fixture");
    expect_invalid_argument(
        [] {
            std::string branch = "bad";
            branch.push_back('\0');
            branch += "branch";
            static_cast<void>(
                make_validated_exact_git_branch_fixture_for_test(
                    std::move(branch)));
        },
        "NUL exact branch fixture");

    const auto canonical_default = default_key(
        "HTTPS://EXAMPLE.COM:443/repo.git");
    require(canonical_default ==
                default_key("https://example.com/repo.git"),
            "Canonical remote was not the key identity.");
    require(canonical_default !=
                default_key("https://example.com/other.git"),
            "Different remotes collapsed into one key.");
    require(canonical_default !=
                exact_key("https://example.com/repo.git", "main"),
            "Default HEAD and exact branch keys collapsed.");
    require(exact_key("https://example.com/repo.git", "main") !=
                exact_key("https://example.com/repo.git", "develop"),
            "Distinct exact branch keys collapsed.");

    const auto request = default_request(
        "HTTPS://EXAMPLE.COM:443/repo.git");
    require(request.key().remote().canonical_url() ==
                    "https://example.com/repo.git" &&
                request.source().source().source_location() ==
                    "HTTPS://EXAMPLE.COM:443/repo.git",
            "Request lost canonical network key or source identity.");

    expect_invalid_argument(
        [] {
            VcsSourceIdentity source = VcsSourceIdentity::make(
                VcsKind::Svn,
                "https://example.com/repo",
                VcsSelector::default_head());
            static_cast<void>(
                make_authority_approved_git_source_identity_fixture_for_test(
                    std::move(source)));
        },
        "Non-Git authority fixture");
    expect_invalid_argument(
        [] {
            VcsSourceIdentity source = VcsSourceIdentity::make(
                VcsKind::Git,
                "https://example.com/repo.git",
                VcsSelector::default_head());
            static_cast<void>(ValidatedGitRemoteRevisionRequest::make(
                make_authority_approved_git_source_identity_fixture_for_test(
                    std::move(source)),
                default_key("https://example.com/other.git")));
        },
        "Mismatched request remote");
    expect_invalid_argument(
        [] {
            VcsSourceIdentity source = VcsSourceIdentity::make(
                VcsKind::Git,
                "https://example.com/repo.git",
                VcsSelector::branch("main"));
            static_cast<void>(ValidatedGitRemoteRevisionRequest::make(
                make_authority_approved_git_source_identity_fixture_for_test(
                    std::move(source)),
                exact_key(
                    "https://example.com/repo.git", "develop")));
        },
        "Mismatched request branch");
    expect_invalid_argument(
        [] {
            VcsSourceIdentity source = VcsSourceIdentity::make(
                VcsKind::Git,
                "https://example.com/repo.git",
                VcsSelector::tag("v1"));
            static_cast<void>(ValidatedGitRemoteRevisionRequest::make(
                make_authority_approved_git_source_identity_fixture_for_test(
                    std::move(source)),
                default_key("https://example.com/repo.git")));
        },
        "Unsupported request selector");
}

void test_result_arms_are_distinct() {
    const auto key = default_key("https://example.com/repo.git");
    const GitRemoteRevisionObservationResult timeout =
        GitRemoteRevisionTimeout{key};
    const GitRemoteRevisionObservationResult process_failure =
        GitRemoteRevisionProcessFailure{
            key, BoundedProcessSignaled{SIGTERM}};
    const GitRemoteRevisionObservationResult git_exit_failure =
        GitRemoteRevisionGitExitFailure{key, 128};
    const GitRemoteRevisionObservationResult capture_failure =
        GitRemoteRevisionCaptureLimitExceeded{key, 16U * 1024U};

    static_cast<void>(
        expect_result<GitRemoteRevisionTimeout>(timeout, "Timeout arm"));
    const auto& process = expect_result<GitRemoteRevisionProcessFailure>(
        process_failure, "ProcessFailure arm");
    require(
        std::get<BoundedProcessSignaled>(process.cause).signal_number ==
            SIGTERM,
        "Process failure cause was not retained.");
    require(expect_result<GitRemoteRevisionGitExitFailure>(
                git_exit_failure, "GitExitFailure arm")
                    .exit_code == 128,
            "Git exit code was not retained.");
    require(expect_result<GitRemoteRevisionCaptureLimitExceeded>(
                capture_failure, "CaptureLimitExceeded arm")
                    .capture_limit == 16U * 1024U,
            "Capture limit was not retained.");
}

void test_default_head_success_and_status_mapping() {
    const auto request = default_request();
    const auto& symbolic_sha1 = expect_observed(
        request,
        symref_record("refs/heads/main") + oid_record(SHA1, "HEAD"),
        SHA1,
        "Symbolic SHA-1 HEAD");
    require(symbolic_sha1.revision().value().git_object_format() != nullptr &&
                *symbolic_sha1.revision().value().git_object_format() ==
                    GitObjectFormat::Sha1,
            "Symbolic SHA-1 object format differs.");
    static_cast<void>(expect_observed(
        request, oid_record(SHA1, "HEAD"), SHA1,
        "Detached SHA-1 HEAD"));

    const auto& symbolic_sha256 = expect_observed(
        request,
        symref_record("refs/heads/main") + oid_record(SHA256, "HEAD"),
        SHA256,
        "Symbolic SHA-256 HEAD");
    require(
        symbolic_sha256.revision().value().git_object_format() != nullptr &&
            *symbolic_sha256.revision().value().git_object_format() ==
                GitObjectFormat::Sha256,
        "Symbolic SHA-256 object format differs.");
    static_cast<void>(expect_observed(
        request, oid_record(SHA256, "HEAD"), SHA256,
        "Detached SHA-256 HEAD"));

    expect_malformed(
        request,
        0,
        "",
        GitRemoteRevisionMalformedOutputReason::EmptyOutput,
        "Status 0 empty output");
    const auto ref_not_found = parse_git_remote_revision_observation(
        request, 2, "");
    require(expect_result<GitRemoteRevisionRefNotFound>(
                ref_not_found, "Status 2 empty output")
                    .key == request.key(),
            "RefNotFound key differs.");
    expect_malformed(
        request,
        2,
        oid_record(SHA1, "HEAD"),
        GitRemoteRevisionMalformedOutputReason::
            RefNotFoundStatusWithOutput,
        "Status 2 nonempty output");
    const auto git_failure = parse_git_remote_revision_observation(
        request, 128, oid_record(SHA1, "HEAD"));
    require(expect_result<GitRemoteRevisionGitExitFailure>(
                git_failure, "Nonzero Git exit")
                    .exit_code == 128,
            "Nonzero Git exit was parsed as success.");
}

void test_default_head_malformed_transcripts() {
    const auto request = default_request();
    expect_malformed(
        request,
        0,
        oid_record(std::string(40, 'A'), "HEAD"),
        GitRemoteRevisionMalformedOutputReason::InvalidRecord,
        "Uppercase HEAD OID");
    for(std::size_t width : {39U, 41U, 63U, 65U}) {
        expect_malformed(
            request,
            0,
            oid_record(std::string(width, 'a'), "HEAD"),
            GitRemoteRevisionMalformedOutputReason::InvalidRecord,
            "Bad HEAD OID width " + std::to_string(width));
    }
    std::string bad_hex(40, 'a');
    bad_hex[20] = 'g';
    expect_malformed(
        request,
        0,
        oid_record(bad_hex, "HEAD"),
        GitRemoteRevisionMalformedOutputReason::InvalidRecord,
        "Non-hex HEAD OID");
    expect_malformed(
        request,
        0,
        oid_record(SHA1, "refs/heads/main"),
        GitRemoteRevisionMalformedOutputReason::WrongRef,
        "HEAD OID wrong ref");
    expect_malformed(
        request,
        0,
        symref_record("refs/tags/v1") + oid_record(SHA1, "HEAD"),
        GitRemoteRevisionMalformedOutputReason::InvalidSymbolicHeadTarget,
        "Unexpected symbolic HEAD namespace");
    expect_malformed(
        request,
        0,
        symref_record("refs/heads/") + oid_record(SHA1, "HEAD"),
        GitRemoteRevisionMalformedOutputReason::InvalidSymbolicHeadTarget,
        "Empty symbolic HEAD branch tail");
    expect_malformed(
        request,
        0,
        oid_record(SHA1, "HEAD") + symref_record("refs/heads/main"),
        GitRemoteRevisionMalformedOutputReason::WrongRecordOrder,
        "Reversed symbolic HEAD records");
    expect_malformed(
        request,
        0,
        symref_record("refs/heads/main") +
            symref_record("refs/heads/other") +
            oid_record(SHA1, "HEAD"),
        GitRemoteRevisionMalformedOutputReason::UnexpectedSymref,
        "Multiple symbolic HEAD records");

    expect_malformed(
        request,
        0,
        std::string(SHA1) + " HEAD\n",
        GitRemoteRevisionMalformedOutputReason::InvalidRecord,
        "Space delimiter");
    expect_malformed(
        request,
        0,
        std::string(SHA1) + "\tHEAD\r\n",
        GitRemoteRevisionMalformedOutputReason::UnexpectedControlByte,
        "CRLF transcript");
    expect_malformed(
        request,
        0,
        std::string(SHA1) + "\tHEAD",
        GitRemoteRevisionMalformedOutputReason::MissingFinalLineFeed,
        "Partial final line");

    std::string nul = std::string(SHA1) + "\tHE";
    nul.push_back('\0');
    nul += "AD\n";
    expect_malformed(
        request,
        0,
        nul,
        GitRemoteRevisionMalformedOutputReason::UnexpectedControlByte,
        "NUL transcript");
    std::string c0 = std::string(SHA1) + "\tHE";
    c0.push_back('\x01');
    c0 += "AD\n";
    expect_malformed(
        request,
        0,
        c0,
        GitRemoteRevisionMalformedOutputReason::UnexpectedControlByte,
        "C0 transcript");
    std::string del = std::string(SHA1) + "\tHE";
    del.push_back('\x7f');
    del += "AD\n";
    expect_malformed(
        request,
        0,
        del,
        GitRemoteRevisionMalformedOutputReason::UnexpectedControlByte,
        "DEL transcript");
}

void test_default_head_ambiguous_transcripts() {
    const auto request = default_request();
    expect_ambiguous(
        request,
        oid_record(SHA1, "HEAD") + oid_record(SHA1, "HEAD"),
        GitRemoteRevisionAmbiguousOutputReason::DuplicateExpectedRecord,
        2,
        "Duplicate detached HEAD");
    expect_ambiguous(
        request,
        oid_record(SHA1, "HEAD") +
            oid_record(SHA1, "refs/archive/HEAD"),
        GitRemoteRevisionAmbiguousOutputReason::ExtraOidRecord,
        2,
        "Extra HEAD tail-match ref");
    expect_ambiguous(
        request,
        symref_record("refs/heads/main") + oid_record(SHA1, "HEAD") +
            oid_record(SHA1, "refs/archive/HEAD"),
        GitRemoteRevisionAmbiguousOutputReason::ExtraOidRecord,
        3,
        "Third HEAD record");
}

void test_exact_branch_success() {
    const auto request = exact_request();
    static_cast<void>(expect_observed(
        request,
        oid_record(SHA1, "refs/heads/exact"),
        SHA1,
        "Exact branch SHA-1"));
    const auto& sha256 = expect_observed(
        request,
        oid_record(SHA256, "refs/heads/exact"),
        SHA256,
        "Exact branch SHA-256");
    require(sha256.revision().value().git_object_format() != nullptr &&
                *sha256.revision().value().git_object_format() ==
                    GitObjectFormat::Sha256,
            "Exact branch SHA-256 object format differs.");
}

void test_exact_branch_rejections() {
    const auto request = exact_request();
    expect_malformed(
        request,
        0,
        oid_record(SHA1, "refs/heads/other"),
        GitRemoteRevisionMalformedOutputReason::WrongRef,
        "Wrong exact branch");
    expect_malformed(
        request,
        0,
        oid_record(SHA1, "refs/archive/refs/heads/exact"),
        GitRemoteRevisionMalformedOutputReason::WrongRef,
        "Tail-match other namespace");
    expect_malformed(
        request,
        0,
        oid_record(SHA1, "refs/heads/archive/refs/heads/exact"),
        GitRemoteRevisionMalformedOutputReason::WrongRef,
        "Nested branch tail match");
    expect_ambiguous(
        request,
        oid_record(SHA1, "refs/heads/exact") +
            oid_record(SHA1, "refs/heads/exact"),
        GitRemoteRevisionAmbiguousOutputReason::DuplicateExpectedRecord,
        2,
        "Duplicate same exact branch OID");
    expect_ambiguous(
        request,
        oid_record(SHA1, "refs/heads/exact") +
            oid_record(std::string(40, 'a'), "refs/heads/exact"),
        GitRemoteRevisionAmbiguousOutputReason::DuplicateExpectedRecord,
        2,
        "Duplicate different exact branch OID");
    expect_ambiguous(
        request,
        oid_record(SHA1, "refs/heads/exact") +
            oid_record(SHA1, "refs/heads/other"),
        GitRemoteRevisionAmbiguousOutputReason::ExtraOidRecord,
        2,
        "Extra exact branch record");
    expect_malformed(
        request,
        0,
        symref_record("refs/heads/exact", "refs/heads/exact"),
        GitRemoteRevisionMalformedOutputReason::UnexpectedSymref,
        "Exact branch symref");
    expect_malformed(
        request,
        0,
        oid_record(SHA1, "refs/heads/exact^{}"),
        GitRemoteRevisionMalformedOutputReason::WrongRef,
        "Exact branch peeled-looking record");
    expect_malformed(
        request,
        0,
        oid_record(std::string(40, 'A'), "refs/heads/exact"),
        GitRemoteRevisionMalformedOutputReason::InvalidRecord,
        "Exact branch uppercase OID");
    expect_malformed(
        request,
        0,
        std::string(SHA1) + "\trefs/heads/exact",
        GitRemoteRevisionMalformedOutputReason::MissingFinalLineFeed,
        "Exact branch partial line");

    std::string control = std::string(SHA1) + "\trefs/heads/ex";
    control.push_back('\x01');
    control += "act\n";
    expect_malformed(
        request,
        0,
        control,
        GitRemoteRevisionMalformedOutputReason::UnexpectedControlByte,
        "Exact branch control byte");
}

bool environment_contains_name(
    const std::vector<std::string>& environment,
    std::string_view name) {
    const std::string prefix = std::string(name) + "=";
    for(const std::string& assignment : environment) {
        if(assignment.starts_with(prefix)) return true;
    }
    return false;
}

void test_trusted_git_observer_environment() {
    const std::vector<std::string> controlled_names{
        "http_proxy",
        "https_proxy",
        "all_proxy",
        "no_proxy",
        "HTTP_PROXY",
        "HTTPS_PROXY",
        "ALL_PROXY",
        "NO_PROXY",
        "SSL_CERT_FILE",
        "SSL_CERT_DIR",
        "GIT_SSL_CAINFO",
        "GIT_SSL_CAPATH",
        "HOME",
        "XDG_CONFIG_HOME",
        "XDG_STATE_HOME",
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_CONFIG",
        "GIT_CONFIG_PARAMETERS",
        "GIT_CONFIG_COUNT",
        "GIT_CONFIG_KEY_0",
        "GIT_CONFIG_VALUE_0",
        "GIT_EXEC_PATH",
        "GIT_SSH",
        "GIT_SSH_COMMAND",
        "GIT_TRACE",
        "GIT_TRACE_CURL",
        "GIT_PROXY_COMMAND",
        "GIT_SSL_NO_VERIFY",
        "CURL_CA_BUNDLE",
        "LD_PRELOAD",
        "LD_LIBRARY_PATH",
        "BASH_ENV",
        "ENV",
        "SHELL",
    };
    ScopedEnvironment environment_guard(controlled_names);

    const std::array<std::pair<std::string_view, std::string_view>, 8>
        proxies{{
            {"http_proxy", "http://lower-http.invalid/path with spaces"},
            {"https_proxy", "http://lower-https.invalid:8443"},
            {"all_proxy", "socks5://lower-all.invalid:1080"},
            {"no_proxy", "127.0.0.1,localhost"},
            {"HTTP_PROXY", "http://upper-http.invalid:8080"},
            {"HTTPS_PROXY", "http://upper-https.invalid:9443"},
            {"ALL_PROXY", "socks5://upper-all.invalid:1081"},
            {"NO_PROXY", "localhost,127.0.0.1"},
        }};
    for(const auto& [name, value] : proxies) {
        environment_guard.set(std::string(name), std::string(value));
    }
    environment_guard.set("SSL_CERT_FILE", "/tmp/observer-cert.pem");
    environment_guard.set(
        "SSL_CERT_DIR", "/tmp/observer-certs:/opt/observer-certs");
    environment_guard.set("GIT_SSL_CAINFO", "/tmp/observer-git-ca.pem");
    environment_guard.set("GIT_SSL_CAPATH", "/tmp/observer-git-ca");

    for(std::string_view forbidden : {
            "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "GIT_DIR",
            "GIT_WORK_TREE", "GIT_CONFIG", "GIT_CONFIG_PARAMETERS",
            "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0",
            "GIT_EXEC_PATH", "GIT_SSH", "GIT_SSH_COMMAND", "GIT_TRACE",
            "GIT_TRACE_CURL", "GIT_PROXY_COMMAND", "GIT_SSL_NO_VERIFY",
            "CURL_CA_BUNDLE", "LD_PRELOAD", "LD_LIBRARY_PATH", "BASH_ENV",
            "ENV", "SHELL"}) {
        environment_guard.set(
            std::string(forbidden), "/tmp/forbidden-observer-value");
    }

    const std::vector<std::string> actual =
        trusted_git_process_environment(
            TrustedGitProcessEnvironmentMode::ReadOnlyObservation);
    std::vector<std::string> expected{
        "PATH=/usr/bin:/bin",
        "LC_ALL=C",
        "LANG=C",
        "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_SYSTEM=/dev/null",
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_TERMINAL_PROMPT=0",
        "GIT_ASKPASS=/bin/false",
        "SSH_ASKPASS=/bin/false",
        "GIT_PAGER=cat",
        "PAGER=cat",
        "GIT_ATTR_NOSYSTEM=1",
        "GIT_OPTIONAL_LOCKS=0",
    };
    for(const auto& [name, value] : proxies) {
        expected.emplace_back(
            std::string(name) + "=" + std::string(value));
    }
    expected.insert(
        expected.end(),
        {"SSL_CERT_FILE=/tmp/observer-cert.pem",
         "SSL_CERT_DIR=/tmp/observer-certs:/opt/observer-certs",
         "GIT_SSL_CAINFO=/tmp/observer-git-ca.pem",
         "GIT_SSL_CAPATH=/tmp/observer-git-ca"});
    require(actual == expected,
            "Observer Git environment differs from the exact allowlist");
    require(!environment_contains_name(actual, "HOME") &&
                !environment_contains_name(actual, "XDG_CONFIG_HOME") &&
                !environment_contains_name(actual, "GIT_CONFIG_COUNT") &&
                !environment_contains_name(actual, "GIT_EXEC_PATH") &&
                !environment_contains_name(actual, "CURL_CA_BUNDLE") &&
                !environment_contains_name(actual, "LD_PRELOAD"),
            "Forbidden ambient state reached observer Git envp");

    const std::string relative_secret = "relative/secret-observer-ca.pem";
    environment_guard.set("SSL_CERT_FILE", relative_secret);
    try {
        static_cast<void>(trusted_git_process_environment(
            TrustedGitProcessEnvironmentMode::ReadOnlyObservation));
    } catch(const std::runtime_error& error) {
        require(
            std::string_view(error.what()).find("SSL_CERT_FILE") !=
                    std::string_view::npos &&
                std::string_view(error.what()).find(relative_secret) ==
                    std::string_view::npos,
            "Relative CA rejection exposed the path value");
        return;
    }
    throw std::runtime_error("Relative observer CA path was accepted");
}

void test_observer_environment_setup_failure_mapping() {
    const std::vector<std::string> ca_names{
        "SSL_CERT_FILE", "SSL_CERT_DIR", "GIT_SSL_CAINFO",
        "GIT_SSL_CAPATH"};
    ScopedEnvironment environment_guard(ca_names);
    for(const std::string& name : ca_names)
        environment_guard.unset(name);
    environment_guard.set(
        "GIT_SSL_CAINFO", "relative/observer-fixture-secret-ca.pem");

    const auto result = observe_git_remote_revision(default_request());
    const auto& process_failure =
        expect_result<GitRemoteRevisionProcessFailure>(
            result, "Observer trusted environment setup failure");
    const auto& cause = std::get<BoundedProcessLaunchOrSetupFailure>(
        process_failure.cause);
    require(
        cause.stage == BoundedProcessLaunchStage::InvocationValidation &&
            cause.error_number == EINVAL,
        "Trusted environment failure was flattened to a Git exit");
}

void test_trusted_git_observer_fixed_arguments() {
    const std::vector<std::string> expected{
        "--no-pager",
        "--git-dir=/dev/null",
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.sshCommand=/bin/false",
        "-c",
        "credential.helper=",
        "-c",
        "credential.interactive=false",
        "-c",
        "credential.username=",
        "-c",
        "core.askPass=/bin/false",
        "-c",
        "http.emptyAuth=false",
        "-c",
        "http.proactiveAuth=none",
        "-c",
        "http.delegation=none",
        "-c",
        "http.extraHeader=",
        "-c",
        "http.cookieFile=",
        "-c",
        "http.saveCookies=false",
        "-c",
        "http.followRedirects=false",
        "-c",
        "http.sslVerify=true",
        "-c",
        "protocol.allow=never",
        "-c",
        "protocol.https.allow=always",
        "-c",
        "protocol.http.allow=never",
        "-c",
        "protocol.file.allow=never",
        "-c",
        "protocol.ext.allow=never",
        "-c",
        "protocol.ssh.allow=never",
        "-c",
        "protocol.git.allow=never",
        "-c",
        "submodule.recurse=false",
    };
    require(trusted_git_observer_process_arguments() == expected,
            "Observer Git fixed argument profile changed");
}

void test_observer_closed_argv() {
    std::vector<std::string> expected_default =
        trusted_git_observer_process_arguments();
    expected_default.insert(
        expected_default.end(),
        {"ls-remote", "--quiet", "--symref", "--exit-code",
         "https://example.com/repo.git", "HEAD"});
    const auto canonical_request = default_request(
        "HTTPS://EXAMPLE.COM:443/a/../repo.git");
    require(
        git_remote_revision_observer_arguments_fixture_for_test(
            canonical_request) == expected_default,
        "Default HEAD observer argv is not closed over the canonical URL");

    std::vector<std::string> expected_branch =
        trusted_git_observer_process_arguments();
    expected_branch.insert(
        expected_branch.end(),
        {"ls-remote", "--quiet", "--refs", "--branches",
         "--exit-code", "https://example.com:444/repo.git",
         "refs/heads/feature/exact"});
    require(
        git_remote_revision_observer_arguments_fixture_for_test(
            exact_request(
                "feature/exact",
                "HTTPS://EXAMPLE.COM:444/repo.git")) ==
            expected_branch,
        "Exact branch observer argv changed or reused raw URL spelling");
}

void test_observer_execution_composition(const fs::path& executable) {
    const auto request = default_request();

    const auto observed = run_process_fixture(executable, "observed");
    const auto& observed_value =
        expect_result<ObservedGitRemoteRevision>(
            observed, "Executed observed transcript");
    require(observed_value.revision().value().git_commit() != nullptr &&
                *observed_value.revision().value().git_commit() == SHA1,
            "Executed observer transcript lost its OID");

    const auto missing = run_process_fixture(
        executable, "status-2-empty");
    static_cast<void>(expect_result<GitRemoteRevisionRefNotFound>(
        missing, "Executed status 2 empty transcript"));
    const auto status_two_output = run_process_fixture(
        executable, "status-2-output");
    require(
        expect_result<GitRemoteRevisionMalformedOutput>(
            status_two_output, "Executed status 2 nonempty transcript")
                .reason ==
            GitRemoteRevisionMalformedOutputReason::
                RefNotFoundStatusWithOutput,
        "Status 2 with stdout was flattened to RefNotFound");
    const auto git_exit = run_process_fixture(executable, "git-exit");
    require(
        expect_result<GitRemoteRevisionGitExitFailure>(
            git_exit, "Executed Git exit failure")
                .exit_code == 128,
        "Executed Git exit code was not retained");

    const std::array<
        std::pair<
            std::string_view,
            GitRemoteRevisionMalformedOutputReason>,
        4>
        malformed_fixtures{{{"bad-oid",
                             GitRemoteRevisionMalformedOutputReason::InvalidRecord},
                            {"wrong-ref",
                             GitRemoteRevisionMalformedOutputReason::WrongRef},
                            {"control-byte",
                             GitRemoteRevisionMalformedOutputReason::
                                 UnexpectedControlByte},
                            {"partial-output",
                             GitRemoteRevisionMalformedOutputReason::
                                 MissingFinalLineFeed}}};
    for(const auto& [mode, reason] : malformed_fixtures) {
        const auto result = run_process_fixture(
            executable, std::string(mode));
        require(
            expect_result<GitRemoteRevisionMalformedOutput>(
                result, std::string("Executed malformed fixture ") +
                            std::string(mode))
                    .reason == reason,
            "Executed malformed transcript classification differs");
    }

    for(std::string_view mode : {"duplicate", "extra-ref"}) {
        const auto result = run_process_fixture(
            executable, std::string(mode));
        static_cast<void>(expect_result<GitRemoteRevisionAmbiguousOutput>(
            result, std::string("Executed ambiguous fixture ") +
                        std::string(mode)));
    }

    const auto signaled = run_process_fixture(executable, "signaled");
    const auto& signal_failure =
        expect_result<GitRemoteRevisionProcessFailure>(
            signaled, "Executed signal failure");
    require(
        std::get<BoundedProcessSignaled>(signal_failure.cause)
                .signal_number == SIGTERM,
        "Observer signal failure lost its signal number");

    const auto launch_failure =
        observe_git_remote_revision_process_fixture_for_test(
            request,
            "/definitely/not/a/moguet-git-fixture",
            {},
            2s,
            150ms);
    const auto& launch_cause = std::get<
        BoundedProcessLaunchOrSetupFailure>(
        expect_result<GitRemoteRevisionProcessFailure>(
            launch_failure, "Executed launch failure")
            .cause);
    require(
        launch_cause.stage == BoundedProcessLaunchStage::Execve &&
            launch_cause.error_number == ENOENT,
        "Observer launch failure lost its stage or errno");

    const auto io_failure =
        classify_git_remote_revision_bounded_process_fixture_for_test(
            request,
            BoundedCapturedProcessResult{
                {},
                BoundedProcessIoOrWaitFailure{
                    BoundedProcessIoStage::Poll, EIO}});
    const auto& io_cause = std::get<BoundedProcessIoOrWaitFailure>(
        expect_result<GitRemoteRevisionProcessFailure>(
            io_failure, "Mechanical I/O failure mapping")
            .cause);
    require(
        io_cause.stage == BoundedProcessIoStage::Poll &&
            io_cause.error_number == EIO,
        "Observer I/O failure lost its stage or errno");

    const auto timeout_started = std::chrono::steady_clock::now();
    const auto timeout = run_process_fixture(
        executable, "timeout", 300ms, 100ms);
    require(
        std::holds_alternative<GitRemoteRevisionTimeout>(timeout),
        "Mechanical timeout was flattened to another observer result");
    require(
        std::chrono::steady_clock::now() - timeout_started < 2s,
        "Observer timeout fixture waited for the production deadline");

    const auto overflow = run_process_fixture(
        executable, "capture-overflow", 2s, 100ms);
    require(
        expect_result<GitRemoteRevisionCaptureLimitExceeded>(
            overflow, "Executed capture overflow")
                .capture_limit ==
            GIT_REMOTE_OBSERVER_STDOUT_CAPTURE_LIMIT,
        "Observer capture overflow was flattened or changed its limit");
}

void write_text_file(const fs::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output),
            "Failed to create observer config fixture");
    output << contents;
    require(static_cast<bool>(output),
            "Failed to write observer config fixture");
}

void test_observer_git_config_isolation() {
    TemporaryTree fixture;
    const fs::path home = fixture.path() / "home";
    const fs::path xdg = fixture.path() / "xdg";
    const fs::path repository = fixture.path() / "repository";
    fs::create_directories(home);
    fs::create_directories(xdg / "git");
    fs::create_directories(repository / ".git");
    const std::string malicious_config =
        "[url \"file:///tmp/observer-policy-escape\"]\n"
        "    insteadOf = https://observer.invalid/repo\n"
        "[credential]\n"
        "    helper = /tmp/observer-credential-helper\n";
    write_text_file(home / ".gitconfig", malicious_config);
    write_text_file(xdg / "git" / "config", malicious_config);
    write_text_file(repository / ".git" / "config", malicious_config);

    const std::vector<std::string> names{
        "HOME", "XDG_CONFIG_HOME", "GIT_DIR",
        "GIT_WORK_TREE", "GIT_CONFIG", "GIT_CONFIG_PARAMETERS",
        "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0",
        "SSL_CERT_FILE", "SSL_CERT_DIR", "GIT_SSL_CAINFO",
        "GIT_SSL_CAPATH"};
    ScopedEnvironment environment_guard(names);
    environment_guard.set("HOME", home.string());
    environment_guard.set("XDG_CONFIG_HOME", xdg.string());
    environment_guard.set("GIT_DIR", (repository / ".git").string());
    environment_guard.set("GIT_WORK_TREE", repository.string());
    environment_guard.set("GIT_CONFIG", (home / ".gitconfig").string());
    environment_guard.set(
        "GIT_CONFIG_PARAMETERS",
        "'url.file:///tmp/parameters.insteadOf'='https://observer.invalid/repo'");
    environment_guard.set("GIT_CONFIG_COUNT", "1");
    environment_guard.set(
        "GIT_CONFIG_KEY_0",
        "url.file:///tmp/count.insteadOf");
    environment_guard.set(
        "GIT_CONFIG_VALUE_0", "https://observer.invalid/repo");
    environment_guard.unset("SSL_CERT_FILE");
    environment_guard.unset("SSL_CERT_DIR");
    environment_guard.unset("GIT_SSL_CAINFO");
    environment_guard.unset("GIT_SSL_CAPATH");

    int directory_descriptor = open(
        repository.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(directory_descriptor >= 0,
            "Failed to open observer config cwd");
    OwnedDescriptor directory(directory_descriptor);
    int input_descriptor = open(
        "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(input_descriptor >= 0,
            "Failed to open observer config stdin");
    OwnedDescriptor input(input_descriptor);

    std::vector<std::string> arguments =
        trusted_git_observer_process_arguments();
    arguments.insert(
        arguments.end(), {"config", "--list", "--show-origin"});
    ExplicitProcessInvocation invocation{
        "/usr/bin/git", std::move(arguments),
        trusted_git_process_environment(
            TrustedGitProcessEnvironmentMode::ReadOnlyObservation)};
    invocation.working_directory_fd = directory.get();
    invocation.standard_input_fd = input.get();
    const auto result = capture_bounded_explicit_process_output_raw(
        invocation,
        BoundedProcessPolicy{
            std::chrono::seconds(5), std::chrono::milliseconds(500),
            GIT_REMOTE_OBSERVER_STDOUT_CAPTURE_LIMIT, true});
    const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
    require(exited != nullptr && exited->exit_code == 0,
            "Isolated observer Git config characterization failed");
    require(result.output.find("observer-policy-escape") == std::string::npos &&
                result.output.find("observer-credential-helper") ==
                    std::string::npos &&
                result.output.find("parameters.insteadof") ==
                    std::string::npos &&
                result.output.find("count.insteadof") == std::string::npos,
            "Ambient HOME/XDG/local/GIT_CONFIG state reached observer Git");
    require(result.output.find("protocol.https.allow=always") !=
                    std::string::npos &&
                result.output.find("protocol.http.allow=never") !=
                    std::string::npos &&
                result.output.find("protocol.file.allow=never") !=
                    std::string::npos &&
                result.output.find("http.followredirects=false") !=
                    std::string::npos,
            "Observer Git did not receive the HTTPS-only fixed profile");
}

void test_exact_branch_production_validation() {
    for(std::string_view accepted : {"main", "feature/x", "@"}) {
        const auto result = validate_exact_git_branch(accepted);
        const auto& branch =
            expect_branch_validation_result<ValidatedExactGitBranch>(
                result, accepted);
        require(branch.name() == accepted,
                "Git changed the accepted exact branch spelling");
    }

    for(std::string_view rejected : {
            "-bad", "@{-1}", "foo.lock", "foo..bar", "a b", "HEAD"}) {
        const auto result = validate_exact_git_branch(rejected);
        const auto& invalid =
            expect_branch_validation_result<InvalidExactGitBranch>(
                result, rejected);
        require(
            invalid.reason == InvalidExactGitBranchReason::GitRejected &&
                invalid.git_exit_code.has_value() &&
                *invalid.git_exit_code != 0,
            "Git syntax rejection was not kept as typed invalid input");
    }

    const auto empty = validate_exact_git_branch("");
    require(
        expect_branch_validation_result<InvalidExactGitBranch>(
            empty, "empty branch")
                .reason == InvalidExactGitBranchReason::Empty,
        "Empty branch resource preflight changed");
    const std::string embedded_nul{'m', 'a', 'i', 'n', '\0', 'x'};
    const auto nul = validate_exact_git_branch(embedded_nul);
    require(
        expect_branch_validation_result<InvalidExactGitBranch>(
            nul, "NUL branch")
                .reason == InvalidExactGitBranchReason::EmbeddedNul,
        "NUL branch resource preflight changed");
    const auto oversized = validate_exact_git_branch(
        std::string(VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES + 1U, 'a'));
    require(
        expect_branch_validation_result<InvalidExactGitBranch>(
            oversized, "oversized branch")
                .reason == InvalidExactGitBranchReason::InputTooLong,
        "Branch input resource bound changed");
    const std::string boundary_name(
        VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES, 'a');
    const auto boundary = validate_exact_git_branch(boundary_name);
    require(
        expect_branch_validation_result<ValidatedExactGitBranch>(
            boundary, "boundary-sized branch")
                .name()
                .size() == VALIDATED_EXACT_GIT_BRANCH_MAX_INPUT_BYTES,
        "Exact branch input/capture boundary was not inclusive");

    const auto exact =
        classify_exact_git_branch_exited_process_fixture_for_test(
            "main", 0, "main\n");
    require(
        expect_branch_validation_result<ValidatedExactGitBranch>(
            exact, "exact branch transcript")
                .name() == "main",
        "Exact branch success transcript was rejected");
    const std::vector<std::string> malformed_transcripts{
        "different\n",
        "main\nextra\n",
        "main",
        "main\r\n",
        std::string{'m', 'a', 'i', '\x01', 'n', '\n'},
    };
    for(std::string malformed : malformed_transcripts) {
        const auto result =
            classify_exact_git_branch_exited_process_fixture_for_test(
                "main", 0, std::move(malformed));
        require(
            expect_branch_validation_result<
                ExactGitBranchValidationProcessFailure>(
                result, "malformed branch transcript")
                    .reason ==
                ExactGitBranchValidationProcessFailureReason::
                    UnexpectedOutput,
            "Status-zero malformed branch transcript was accepted");
    }
    const auto rejected_with_output =
        classify_exact_git_branch_exited_process_fixture_for_test(
            "main", 128, "different\n");
    require(
        expect_branch_validation_result<InvalidExactGitBranch>(
            rejected_with_output, "nonzero branch transcript")
                .reason == InvalidExactGitBranchReason::GitRejected,
        "Nonzero Git status was classified from stderr/stdout text");
}

void test_exact_branch_bounded_process_results() {
    using Reason = ExactGitBranchValidationProcessFailureReason;
    struct Case {
        BoundedProcessOutcome outcome;
        std::optional<Reason> failure_reason;
        std::optional<int> detail;
    };
    const std::vector<Case> cases{
        {BoundedProcessExited{0}, std::nullopt, std::nullopt},
        {BoundedProcessExited{23}, std::nullopt, std::nullopt},
        {BoundedProcessSignaled{SIGTERM}, Reason::Signaled, SIGTERM},
        {BoundedProcessLaunchOrSetupFailure{BoundedProcessLaunchStage::Execve, ENOENT}, Reason::LaunchOrSetup, ENOENT},
        {BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::Poll, EIO}, Reason::IoOrWait, EIO},
        {BoundedProcessTimedOut{}, Reason::TimedOut, std::nullopt},
        {BoundedProcessCaptureLimitExceeded{4}, Reason::CaptureLimitExceeded, 4},
    };
    for(const auto& test_case : cases) {
        for(const bool cancelled : {false, true}) {
            unsigned calls = 0;
            const BoundedCapturedProcessResult observed{
                "main\n", test_case.outcome, cancelled ? std::optional<int>(SIGINT) : std::nullopt};
            set_exact_git_branch_validation_process_test_hook([&](const auto& invocation, const auto&) {
                require(invocation.executable == "/usr/bin/git" && invocation.arguments.back() == "main",
                        "Bounded fixture bypassed branch producer");
                ++calls;
                return observed;
            });
            const auto result = validate_exact_git_branch("main");
            set_exact_git_branch_validation_process_test_hook({});
            require(calls == 1, "Branch validator did not execute bounded child boundary");
            if(cancelled) {
                const auto& failure = expect_branch_validation_result<ExactGitBranchValidationProcessFailure>(result, "cancelled branch");
                require(failure.reason == Reason::Cancelled && failure.cancellation_signal == SIGINT &&
                            failure.process_outcome == observed.outcome,
                        "Parent cancellation was lost or replaced the child outcome");
            } else if(test_case.failure_reason) {
                const auto& failure = expect_branch_validation_result<ExactGitBranchValidationProcessFailure>(result, "branch process failure");
                require(failure.reason == test_case.failure_reason && failure.detail == test_case.detail &&
                            !failure.cancellation_signal,
                        "Existing branch process failure classification changed");
            } else if(std::get<BoundedProcessExited>(test_case.outcome).exit_code == 0) {
                require(expect_branch_validation_result<ValidatedExactGitBranch>(result, "uncancelled branch").name() == "main",
                        "No-cancel explicit branch success changed");
            } else {
                const auto& invalid = expect_branch_validation_result<InvalidExactGitBranch>(result, "nonzero branch");
                require(invalid.reason == InvalidExactGitBranchReason::GitRejected && invalid.git_exit_code == 23,
                        "Nonzero branch rejection changed");
            }
        }
    }
    std::cout << "Exact branch bounded outcomes / parent cancellation preservation PASS\n";
}

void print_observation_result(
    const GitRemoteRevisionObservationResult& result) {
    if(const auto* observed =
           std::get_if<ObservedGitRemoteRevision>(&result)) {
        const std::string* object_id =
            observed->revision().value().git_commit();
        require(object_id != nullptr,
                "Observed result has no Git commit identity");
        std::cout << "Observed\t" << *object_id << '\n';
        return;
    }
    if(std::holds_alternative<GitRemoteRevisionRefNotFound>(result)) {
        std::cout << "RefNotFound\n";
        return;
    }
    if(std::holds_alternative<GitRemoteRevisionTimeout>(result)) {
        std::cout << "Timeout\n";
        return;
    }
    if(std::holds_alternative<GitRemoteRevisionProcessFailure>(result)) {
        std::cout << "ProcessFailure\n";
        return;
    }
    if(const auto* git_failure =
           std::get_if<GitRemoteRevisionGitExitFailure>(&result)) {
        std::cout << "GitExitFailure\t" << git_failure->exit_code << '\n';
        return;
    }
    if(std::holds_alternative<
           GitRemoteRevisionCaptureLimitExceeded>(result)) {
        std::cout << "CaptureLimitExceeded\n";
        return;
    }
    if(std::holds_alternative<GitRemoteRevisionMalformedOutput>(result)) {
        std::cout << "MalformedOutput\n";
        return;
    }
    if(std::holds_alternative<GitRemoteRevisionAmbiguousOutput>(result)) {
        std::cout << "AmbiguousOutput\n";
        return;
    }
    throw std::logic_error("Unknown observer result in fixture driver");
}

int run_observer_fixture_driver(
    int argc,
    char* argv[],
    const fs::path& executable) {
    require(argc >= 2, "Missing observer fixture driver operation");
    const std::string_view operation(argv[1]);
    if(operation == "--observe-default") {
        require(argc == 3, "Default observer fixture requires one URL");
        print_observation_result(
            observe_git_remote_revision(default_request(argv[2])));
        return 0;
    }
    if(operation == "--observe-branch") {
        require(argc == 4,
                "Branch observer fixture requires URL and branch");
        print_observation_result(observe_git_remote_revision(
            production_exact_request(argv[2], argv[3])));
        return 0;
    }
    if(operation == "--observe-process-fixture") {
        require(argc == 3,
                "Process observer fixture requires one mode");
        const std::string_view mode(argv[2]);
        const auto hard_timeout =
            mode == "timeout" ? 300ms : 2s;
        print_observation_result(run_process_fixture(
            executable, std::string(mode), hard_timeout, 100ms));
        return 0;
    }
    throw std::runtime_error("Unknown observer fixture driver operation");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if(argc >= 2 && argv[1] == PROCESS_FIXTURE_MARKER) {
            return run_observer_process_fixture_child(argc, argv);
        }
        const fs::path executable = fs::canonical(argv[0]);
        if(argc >= 2) {
            return run_observer_fixture_driver(argc, argv, executable);
        }

        test_https_remote_canonicalization();
        test_https_remote_rejections();
        test_selector_key_and_request_model();
        test_result_arms_are_distinct();
        test_default_head_success_and_status_mapping();
        test_default_head_malformed_transcripts();
        test_default_head_ambiguous_transcripts();
        test_exact_branch_success();
        test_exact_branch_rejections();
        test_trusted_git_observer_environment();
        test_observer_environment_setup_failure_mapping();
        test_trusted_git_observer_fixed_arguments();
        test_observer_closed_argv();
        test_observer_execution_composition(executable);
        test_observer_git_config_isolation();
        test_exact_branch_production_validation();
        test_exact_branch_bounded_process_results();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Git remote revision observer tests: all checks passed\n";
    return 0;
}
